#include "flux_ws/ws.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/ssl.h>

#include <stdexec/execution.hpp>

namespace flux::ws {
namespace {

namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

constexpr std::size_t kReadChunkSize = 16 * 1024;

template <typename T>
class EventQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        queue_.push_back(std::move(value));
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    Task<std::optional<T>> pop() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) {
                    T value = std::move(queue_.front());
                    queue_.pop_front();
                    co_return std::optional<T>(std::move(value));
                }
                if (closed_) {
                    co_return std::nullopt;
                }
            }
            co_await flux::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    std::mutex mutex_;
    std::deque<T> queue_;
    bool closed_ = false;
};

std::string ensure_path(std::string value) {
    if (value.empty()) {
        return "/";
    }
    if (value.front() != '/') {
        value.insert(value.begin(), '/');
    }
    return value;
}

std::shared_ptr<ssl::context> create_server_tls_context(const Server::Config &config) {
    if (config.tls_cert_file.empty() || config.tls_private_key_file.empty()) {
        throw WebSocketError("websocket tls server requires cert and private key file");
    }

    auto ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
    ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                     ssl::context::single_dh_use);

    boost::system::error_code ec;
    ctx->use_certificate_chain_file(config.tls_cert_file, ec);
    if (ec) {
        throw WebSocketError("failed to load websocket tls cert: " + ec.message());
    }

    ctx->use_private_key_file(config.tls_private_key_file, ssl::context::pem, ec);
    if (ec) {
        throw WebSocketError("failed to load websocket tls key: " + ec.message());
    }

    if (!config.tls_ca_file.empty()) {
        ctx->load_verify_file(config.tls_ca_file, ec);
        if (ec) {
            throw WebSocketError("failed to load websocket tls CA file: " + ec.message());
        }
    }

    return ctx;
}

std::shared_ptr<ssl::context> create_client_tls_context(const Client::Config &config) {
    auto ctx = std::make_shared<ssl::context>(ssl::context::tls_client);
    boost::system::error_code ec;

    if (config.tls_allow_insecure) {
        ctx->set_verify_mode(ssl::verify_none, ec);
    } else {
        ctx->set_verify_mode(ssl::verify_peer, ec);
        if (ec) {
            throw WebSocketError("failed to set websocket tls verify mode: " + ec.message());
        }

        if (!config.tls_ca_file.empty()) {
            ctx->load_verify_file(config.tls_ca_file, ec);
            if (ec) {
                throw WebSocketError("failed to load websocket client CA file: " + ec.message());
            }
        } else {
            ctx->set_default_verify_paths(ec);
            if (ec) {
                throw WebSocketError(
                    "failed to load default websocket TLS verify paths: " + ec.message());
            }
        }
    }

    return ctx;
}

class SessionHandle {
public:
    virtual ~SessionHandle() = default;
    virtual void start() = 0;
    virtual bool send(StreamChunk chunk) = 0;
    virtual bool close(int code, std::string reason) = 0;
    virtual void stop() = 0;
};

template <class WsStream>
class WebSocketSession final : public SessionHandle,
                               public std::enable_shared_from_this<WebSocketSession<WsStream>> {
public:
    using MessageHandler = std::function<void(Message)>;
    using CloseHandler = std::function<void(int, std::string)>;

    WebSocketSession(WsStream ws,
                     MessageHandler on_message,
                     CloseHandler on_close,
                     std::vector<std::shared_ptr<void>> keep_alive = {})
        : keep_alive_(std::move(keep_alive)),
          ws_(std::move(ws)),
          on_message_(std::move(on_message)),
          on_close_(std::move(on_close)) {}

    ~WebSocketSession() override {
        stop();
        join_reader();
    }

    void start() override {
        auto self = this->shared_from_this();
        reader_thread_ = std::thread([self = std::move(self)] {
            self->read_loop();
        });
    }

    bool send(StreamChunk chunk) override {
        if (stopped_.load(std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(write_mutex_);
        if (stopped_.load(std::memory_order_acquire)) {
            return false;
        }

        if (chunk.is_first_fragment || !outbound_message_active_) {
            ws_.binary(chunk.type == MessageType::binary);
            outbound_message_active_ = true;
        }

        boost::system::error_code ec;
        ws_.write_some(chunk.is_final_fragment, net::buffer(chunk.payload), ec);
        if (ec) {
            outbound_message_active_ = false;
            request_transport_stop();
            if (ec == websocket::error::closed) {
                const auto reason = ws_.reason();
                report_close_once(static_cast<int>(reason.code), std::string(reason.reason));
            } else {
                report_close_once(1006, ec.message());
            }
            return false;
        }

        outbound_message_active_ = !chunk.is_final_fragment;
        return true;
    }

    bool close(int code, std::string reason) override {
        if (stopped_.load(std::memory_order_acquire)) {
            return false;
        }
        report_close_once(code, std::move(reason));
        request_transport_stop();
        return true;
    }

    void stop() override {
        request_transport_stop();
    }

private:
    void read_loop() {
        while (!stopped_.load(std::memory_order_acquire)) {
            beast::flat_buffer buffer;
            boost::system::error_code ec;

            const std::size_t bytes = ws_.read_some(buffer.prepare(kReadChunkSize), ec);
            if (ec) {
                if (ec == websocket::error::closed) {
                    const auto reason = ws_.reason();
                    report_close_once(static_cast<int>(reason.code), std::string(reason.reason));
                } else {
                    report_close_once(1006, ec.message());
                }
                request_transport_stop();
                break;
            }

            buffer.commit(bytes);

            Message message;
            message.type = ws_.got_text() ? MessageType::text : MessageType::binary;
            message.payload = beast::buffers_to_string(buffer.data());
            message.is_first_fragment = next_inbound_is_first_fragment_;
            message.is_final_fragment = ws_.is_message_done();
            next_inbound_is_first_fragment_ = message.is_final_fragment;

            if (on_message_) {
                on_message_(std::move(message));
            }

            buffer.consume(buffer.size());
        }
    }

    void request_transport_stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        boost::system::error_code ignored;
        auto &lowest = beast::get_lowest_layer(ws_);
        lowest.socket().shutdown(tcp::socket::shutdown_both, ignored);
        lowest.socket().close(ignored);
    }

    void report_close_once(int code, std::string reason) {
        bool expected = false;
        if (!close_reported_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        if (on_close_) {
            on_close_(code, std::move(reason));
        }
    }

    void join_reader() {
        if (!reader_thread_.joinable()) {
            return;
        }

        if (reader_thread_.get_id() == std::this_thread::get_id()) {
            reader_thread_.detach();
            return;
        }

        reader_thread_.join();
    }

    std::vector<std::shared_ptr<void>> keep_alive_;
    WsStream ws_;
    MessageHandler on_message_;
    CloseHandler on_close_;

    std::mutex write_mutex_;
    std::thread reader_thread_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> close_reported_{false};
    bool outbound_message_active_ = false;
    bool next_inbound_is_first_fragment_ = true;
};

struct ServerState;

std::shared_ptr<SessionHandle> make_server_plain_session(tcp::socket socket,
                                                         std::uint64_t connection_id,
                                                         const std::shared_ptr<ServerState> &state);

std::shared_ptr<SessionHandle> make_server_tls_session(tcp::socket socket,
                                                       std::uint64_t connection_id,
                                                       const std::shared_ptr<ServerState> &state,
                                                       const std::shared_ptr<ssl::context> &tls_ctx);

struct ServerState {
    explicit ServerState(Server::Config cfg) : config(std::move(cfg)) {}

    Server::Config config;
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<SessionHandle>> connections;
    std::uint64_t next_connection_id = 1;
    int bound_port = 0;
    bool started = false;
    bool stopping = false;

    std::shared_ptr<net::io_context> accept_ioc;
    std::shared_ptr<tcp::acceptor> acceptor;
    std::shared_ptr<ssl::context> tls_ctx;
    std::thread accept_thread;

    EventQueue<ServerEvent> events;
};

std::optional<tcp::endpoint> parse_listen_endpoint(std::string_view host, int port) {
    if (port < 0 || port > 65535) {
        return std::nullopt;
    }

    boost::system::error_code ec;
    const auto address = net::ip::make_address(host, ec);
    if (ec) {
        return std::nullopt;
    }

    return tcp::endpoint(address, static_cast<unsigned short>(port));
}

void emit_server_close(const std::shared_ptr<ServerState> &state,
                       std::uint64_t connection_id,
                       int code,
                       std::string reason) {
    if (!state) {
        return;
    }

    ServerEvent event;
    event.kind = ServerEvent::Kind::close;
    event.connection_id = connection_id;
    event.close_code = code;
    event.close_reason = std::move(reason);
    state->events.push(std::move(event));
}

void emit_server_message(const std::shared_ptr<ServerState> &state,
                         std::uint64_t connection_id,
                         Message message) {
    if (!state) {
        return;
    }

    ServerEvent event;
    event.kind = ServerEvent::Kind::message;
    event.connection_id = connection_id;
    event.message = std::move(message);
    state->events.push(std::move(event));
}

void remove_server_connection(const std::shared_ptr<ServerState> &state,
                              std::uint64_t connection_id,
                              int code,
                              std::string reason) {
    if (!state) {
        return;
    }

    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto it = state->connections.find(connection_id);
        if (it != state->connections.end()) {
            state->connections.erase(it);
            removed = true;
        }
    }

    if (removed) {
        emit_server_close(state, connection_id, code, std::move(reason));
    }
}

std::shared_ptr<SessionHandle> make_server_plain_session(tcp::socket socket,
                                                         std::uint64_t connection_id,
                                                         const std::shared_ptr<ServerState> &state) {
    websocket::stream<beast::tcp_stream> ws(std::move(socket));
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    boost::system::error_code ec;
    ws.accept(ec);
    if (ec) {
        return nullptr;
    }

    auto weak_state = std::weak_ptr<ServerState>(state);
    auto session = std::make_shared<WebSocketSession<decltype(ws)>>(
        std::move(ws),
        [weak_state, connection_id](Message message) mutable {
            if (auto locked = weak_state.lock()) {
                emit_server_message(locked, connection_id, std::move(message));
            }
        },
        [weak_state, connection_id](int code, std::string reason) mutable {
            if (auto locked = weak_state.lock()) {
                remove_server_connection(locked, connection_id, code, std::move(reason));
            }
        });

    return session;
}

std::shared_ptr<SessionHandle> make_server_tls_session(tcp::socket socket,
                                                       std::uint64_t connection_id,
                                                       const std::shared_ptr<ServerState> &state,
                                                       const std::shared_ptr<ssl::context> &tls_ctx) {
    if (!tls_ctx) {
        return nullptr;
    }

    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(std::move(socket), *tls_ctx);
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    boost::system::error_code ec;
    ws.next_layer().handshake(ssl::stream_base::server, ec);
    if (ec) {
        return nullptr;
    }

    ws.accept(ec);
    if (ec) {
        return nullptr;
    }

    auto weak_state = std::weak_ptr<ServerState>(state);
    auto session = std::make_shared<WebSocketSession<decltype(ws)>>(
        std::move(ws),
        [weak_state, connection_id](Message message) mutable {
            if (auto locked = weak_state.lock()) {
                emit_server_message(locked, connection_id, std::move(message));
            }
        },
        [weak_state, connection_id](int code, std::string reason) mutable {
            if (auto locked = weak_state.lock()) {
                remove_server_connection(locked, connection_id, code, std::move(reason));
            }
        });

    return session;
}

void run_accept_loop(const std::shared_ptr<ServerState> &state) {
    while (true) {
        std::shared_ptr<tcp::acceptor> acceptor;
        std::shared_ptr<net::io_context> ioc;
        std::shared_ptr<ssl::context> tls_ctx;
        bool use_tls = false;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->started || state->stopping || !state->acceptor || !state->accept_ioc) {
                break;
            }
            acceptor = state->acceptor;
            ioc = state->accept_ioc;
            tls_ctx = state->tls_ctx;
            use_tls = state->config.use_tls;
        }

        tcp::socket socket(*ioc);
        boost::system::error_code ec;
        acceptor->accept(socket, ec);
        if (ec) {
            if (ec == net::error::would_block || ec == net::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopping || !state->started || ec == net::error::operation_aborted ||
                ec == net::error::bad_descriptor) {
                break;
            }
            continue;
        }

        std::uint64_t connection_id = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->started || state->stopping) {
                boost::system::error_code ignored;
                socket.shutdown(tcp::socket::shutdown_both, ignored);
                socket.close(ignored);
                break;
            }
            connection_id = state->next_connection_id++;
        }

        std::shared_ptr<SessionHandle> session;
        if (use_tls) {
            session = make_server_tls_session(
                std::move(socket), connection_id, state, std::move(tls_ctx));
        } else {
            session = make_server_plain_session(std::move(socket), connection_id, state);
        }

        if (!session) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->started || state->stopping) {
                session->stop();
                break;
            }
            state->connections.insert_or_assign(connection_id, session);
        }

        ServerEvent event;
        event.kind = ServerEvent::Kind::open;
        event.connection_id = connection_id;
        state->events.push(std::move(event));

        session->start();
    }
}

struct ClientState {
    explicit ClientState(Client::Config cfg) : config(std::move(cfg)) {}

    Client::Config config;
    std::mutex mutex;
    std::shared_ptr<SessionHandle> session;
    std::shared_ptr<net::io_context> io_context;
    bool connected = false;
    std::uint64_t generation = 0;
    EventQueue<Client::Event> events;
};

std::shared_ptr<SessionHandle> make_client_plain_session(const Client::Config &config,
                                                         const std::shared_ptr<ClientState> &state,
                                                         std::uint64_t generation,
                                                         const std::shared_ptr<net::io_context> &io_context) {
    tcp::resolver resolver(*io_context);
    boost::system::error_code ec;

    auto endpoints = resolver.resolve(config.address, std::to_string(config.port), ec);
    if (ec) {
        throw WebSocketError("websocket resolve failed: " + ec.message());
    }

    websocket::stream<beast::tcp_stream> ws(*io_context);
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    beast::get_lowest_layer(ws).connect(endpoints, ec);
    if (ec) {
        throw WebSocketError("websocket connect failed: " + ec.message());
    }

    const std::string host = config.host.empty() ? config.address : config.host;
    const std::string path = ensure_path(config.path);
    ws.handshake(host, path, ec);
    if (ec) {
        throw WebSocketError("websocket handshake failed: " + ec.message());
    }

    auto weak_state = std::weak_ptr<ClientState>(state);
    auto session = std::make_shared<WebSocketSession<decltype(ws)>>(
        std::move(ws),
        [weak_state, generation](Message message) mutable {
            if (auto locked = weak_state.lock()) {
                Client::Event event;
                event.kind = Client::Event::Kind::message;
                event.message = std::move(message);
                locked->events.push(std::move(event));
            }
        },
        [weak_state, generation](int code, std::string reason) mutable {
            if (auto locked = weak_state.lock()) {
                {
                    std::lock_guard<std::mutex> lock(locked->mutex);
                    if (locked->generation != generation) {
                        return;
                    }
                    locked->connected = false;
                    locked->session.reset();
                    locked->io_context.reset();
                }

                Client::Event event;
                event.kind = Client::Event::Kind::close;
                event.close_code = code;
                event.close_reason = std::move(reason);
                locked->events.push(std::move(event));
            }
        },
        std::vector<std::shared_ptr<void>>{io_context});

    return session;
}

std::shared_ptr<SessionHandle> make_client_tls_session(const Client::Config &config,
                                                       const std::shared_ptr<ClientState> &state,
                                                       std::uint64_t generation,
                                                       const std::shared_ptr<net::io_context> &io_context) {
    tcp::resolver resolver(*io_context);
    boost::system::error_code ec;

    auto endpoints = resolver.resolve(config.address, std::to_string(config.port), ec);
    if (ec) {
        throw WebSocketError("websocket resolve failed: " + ec.message());
    }

    auto tls_ctx = create_client_tls_context(config);
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(*io_context, *tls_ctx);
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    beast::get_lowest_layer(ws).connect(endpoints, ec);
    if (ec) {
        throw WebSocketError("websocket connect failed: " + ec.message());
    }

    const std::string host = config.host.empty() ? config.address : config.host;

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        throw WebSocketError("failed to set websocket TLS SNI host");
    }

    ws.next_layer().handshake(ssl::stream_base::client, ec);
    if (ec) {
        throw WebSocketError("websocket TLS handshake failed: " + ec.message());
    }

    const std::string path = ensure_path(config.path);
    ws.handshake(host, path, ec);
    if (ec) {
        throw WebSocketError("websocket handshake failed: " + ec.message());
    }

    auto weak_state = std::weak_ptr<ClientState>(state);
    auto session = std::make_shared<WebSocketSession<decltype(ws)>>(
        std::move(ws),
        [weak_state, generation](Message message) mutable {
            if (auto locked = weak_state.lock()) {
                Client::Event event;
                event.kind = Client::Event::Kind::message;
                event.message = std::move(message);
                locked->events.push(std::move(event));
            }
        },
        [weak_state, generation](int code, std::string reason) mutable {
            if (auto locked = weak_state.lock()) {
                {
                    std::lock_guard<std::mutex> lock(locked->mutex);
                    if (locked->generation != generation) {
                        return;
                    }
                    locked->connected = false;
                    locked->session.reset();
                    locked->io_context.reset();
                }

                Client::Event event;
                event.kind = Client::Event::Kind::close;
                event.close_code = code;
                event.close_reason = std::move(reason);
                locked->events.push(std::move(event));
            }
        },
        std::vector<std::shared_ptr<void>>{io_context, tls_ctx});

    return session;
}

} // namespace

struct Server::State : ServerState {
    explicit State(Config cfg) : ServerState(std::move(cfg)) {}
};

struct Client::State : ClientState {
    explicit State(Config cfg) : ClientState(std::move(cfg)) {}
};

Server Server::create(Config config) {
    return Server(std::make_shared<State>(std::move(config)));
}

Server::Server() : state_(std::make_shared<State>(Config{})) {}

Server::Server(std::shared_ptr<State> state) : state_(std::move(state)) {}

Server::~Server() {
    if (!state_) {
        return;
    }
    try {
        stdexec::sync_wait(stop());
    } catch (...) {
    }
}

Server::Server(Server &&other) noexcept : state_(std::move(other.state_)) {}

Server &Server::operator=(Server &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<void> Server::start() {
    if (!state_) {
        throw WebSocketError("websocket server state is null");
    }
    if (!state_->config.runtime) {
        throw WebSocketError("websocket server requires runtime");
    }

    auto endpoint = parse_listen_endpoint(state_->config.host, state_->config.port);
    if (!endpoint.has_value()) {
        throw WebSocketError("invalid websocket server host or port");
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->started) {
            co_return;
        }

        state_->accept_ioc = std::make_shared<net::io_context>(1);
        state_->acceptor = std::make_shared<tcp::acceptor>(*state_->accept_ioc);

        boost::system::error_code ec;
        state_->acceptor->open(endpoint->protocol(), ec);
        if (ec) {
            throw WebSocketError("websocket acceptor open failed: " + ec.message());
        }

        state_->acceptor->set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            throw WebSocketError("websocket acceptor reuse_address failed: " + ec.message());
        }

        state_->acceptor->bind(*endpoint, ec);
        if (ec) {
            throw WebSocketError("websocket acceptor bind failed: " + ec.message());
        }

        state_->acceptor->listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            throw WebSocketError("websocket acceptor listen failed: " + ec.message());
        }

        state_->acceptor->non_blocking(true, ec);
        if (ec) {
            throw WebSocketError("websocket acceptor non_blocking failed: " + ec.message());
        }

        state_->bound_port = static_cast<int>(state_->acceptor->local_endpoint().port());
        state_->stopping = false;
        state_->started = true;

        if (state_->config.use_tls) {
            state_->tls_ctx = create_server_tls_context(state_->config);
        } else {
            state_->tls_ctx.reset();
        }
    }

    auto state = state_;
    state_->accept_thread = std::thread([state = std::move(state)] {
        run_accept_loop(state);
    });

    co_return;
}

Task<void> Server::stop() {
    if (!state_) {
        co_return;
    }

    std::vector<std::shared_ptr<SessionHandle>> sessions;
    std::thread accept_thread;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->started) {
            state_->events.close();
            co_return;
        }

        state_->started = false;
        state_->stopping = true;
        state_->bound_port = 0;

        for (auto &[_, session] : state_->connections) {
            sessions.push_back(session);
        }
        state_->connections.clear();

        if (state_->acceptor) {
            boost::system::error_code ignored;
            state_->acceptor->cancel(ignored);
            state_->acceptor->close(ignored);
        }

        accept_thread = std::move(state_->accept_thread);
    }

    if (accept_thread.joinable()) {
        accept_thread.join();
    }

    for (auto &session : sessions) {
        if (session) {
            (void)session->close(1001, "server stopped");
            session->stop();
        }
    }
    sessions.clear();

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->acceptor.reset();
        state_->accept_ioc.reset();
        state_->tls_ctx.reset();
    }

    state_->events.close();
    co_return;
}

int Server::port() const noexcept {
    if (!state_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->bound_port;
}

bool Server::started() const noexcept {
    if (!state_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->started;
}

Server::task_type Server::next() {
    if (!state_) {
        co_return std::nullopt;
    }
    co_return co_await state_->events.pop();
}

Server::stream_type Server::events() {
    auto state = state_;
    return stream_type([state = std::move(state)]() -> task_type {
        if (!state) {
            co_return std::nullopt;
        }
        co_return co_await state->events.pop();
    });
}

Task<bool> Server::send_text(std::uint64_t connection_id, std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::text;
    chunk.payload = std::string(payload);
    chunk.is_first_fragment = true;
    chunk.is_final_fragment = true;
    co_return co_await send_stream(connection_id, std::move(chunk));
}

Task<bool> Server::send_binary(std::uint64_t connection_id, std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::binary;
    chunk.payload = std::string(payload);
    chunk.is_first_fragment = true;
    chunk.is_final_fragment = true;
    co_return co_await send_stream(connection_id, std::move(chunk));
}

Task<bool> Server::send_stream(std::uint64_t connection_id, StreamChunk chunk) {
    if (!state_) {
        co_return false;
    }

    std::shared_ptr<SessionHandle> session;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->started) {
            co_return false;
        }
        const auto it = state_->connections.find(connection_id);
        if (it == state_->connections.end()) {
            co_return false;
        }
        session = it->second;
    }

    if (!session) {
        co_return false;
    }

    if (state_->config.runtime != nullptr) {
        co_return co_await state_->config.runtime->spawn_blocking(
            [session = std::move(session), chunk = std::move(chunk)]() mutable {
                return session->send(std::move(chunk));
            });
    }

    co_return session->send(std::move(chunk));
}

Task<bool> Server::close(std::uint64_t connection_id, int code, std::string reason) {
    if (!state_) {
        co_return false;
    }

    std::shared_ptr<SessionHandle> session;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->connections.find(connection_id);
        if (it == state_->connections.end()) {
            co_return false;
        }
        session = it->second;
    }

    if (!session) {
        co_return false;
    }

    if (state_->config.runtime != nullptr) {
        co_return co_await state_->config.runtime->spawn_blocking(
            [session = std::move(session), code, reason = std::move(reason)]() mutable {
                return session->close(code, std::move(reason));
            });
    }

    co_return session->close(code, std::move(reason));
}

Client Client::create(Config config) {
    return Client(std::make_shared<State>(std::move(config)));
}

Client::Client() : state_(std::make_shared<State>(Config{})) {}

Client::Client(std::shared_ptr<State> state) : state_(std::move(state)) {}

Client::~Client() {
    if (!state_) {
        return;
    }
    try {
        stdexec::sync_wait(close(1000, "client destroyed"));
    } catch (...) {
    }
}

Client::Client(Client &&other) noexcept : state_(std::move(other.state_)) {}

Client &Client::operator=(Client &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<void> Client::connect() {
    if (!state_) {
        throw WebSocketError("websocket client state is null");
    }

    auto *runtime = state_->config.runtime;
    if (!runtime) {
        throw WebSocketError("websocket client requires runtime");
    }

    std::uint64_t generation = 0;
    auto io_context = std::make_shared<net::io_context>(1);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->connected) {
            co_return;
        }
        generation = ++state_->generation;
    }

    auto session = co_await runtime->spawn_blocking([config = state_->config,
                                                     state = state_,
                                                     generation,
                                                     io_context]() mutable {
        if (config.port <= 0 || config.port > 65535) {
            throw WebSocketError("invalid websocket client port");
        }

        if (config.use_tls) {
            return make_client_tls_session(config, state, generation, io_context);
        }
        return make_client_plain_session(config, state, generation, io_context);
    });

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->generation != generation) {
            if (session) {
                session->stop();
            }
            throw WebSocketError("websocket client connect interrupted by a newer connect attempt");
        }
        state_->session = session;
        state_->io_context = io_context;
        state_->connected = (session != nullptr);
    }

    if (!session) {
        throw WebSocketError("websocket client failed to create session");
    }

    Client::Event open;
    open.kind = Client::Event::Kind::open;
    state_->events.push(std::move(open));

    session->start();
    co_return;
}

Task<void> Client::close(int code, std::string reason) {
    if (!state_) {
        co_return;
    }

    auto *runtime = state_->config.runtime;
    if (!runtime) {
        throw WebSocketError("websocket client requires runtime");
    }

    std::shared_ptr<SessionHandle> session;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->connected || !state_->session) {
            co_return;
        }
        state_->connected = false;
        session = state_->session;
        state_->session.reset();
        state_->io_context.reset();
    }

    co_await runtime->spawn_blocking(
        [session = std::move(session), code, reason = std::move(reason)]() mutable {
            (void)session->close(code, std::move(reason));
            session->stop();
        });

    co_return;
}

bool Client::connected() const noexcept {
    if (!state_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->connected;
}

Client::task_type Client::next() {
    if (!state_) {
        co_return std::nullopt;
    }
    co_return co_await state_->events.pop();
}

Client::stream_type Client::events() {
    auto state = state_;
    return stream_type([state = std::move(state)]() -> task_type {
        if (!state) {
            co_return std::nullopt;
        }
        co_return co_await state->events.pop();
    });
}

Task<bool> Client::send_text(std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::text;
    chunk.payload = std::string(payload);
    chunk.is_first_fragment = true;
    chunk.is_final_fragment = true;
    co_return co_await send_stream(std::move(chunk));
}

Task<bool> Client::send_binary(std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::binary;
    chunk.payload = std::string(payload);
    chunk.is_first_fragment = true;
    chunk.is_final_fragment = true;
    co_return co_await send_stream(std::move(chunk));
}

Task<bool> Client::send_stream(StreamChunk chunk) {
    if (!state_) {
        co_return false;
    }

    auto *runtime = state_->config.runtime;
    if (!runtime) {
        throw WebSocketError("websocket client requires runtime");
    }

    std::shared_ptr<SessionHandle> session;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->connected || !state_->session) {
            co_return false;
        }
        session = state_->session;
    }

    co_return co_await runtime->spawn_blocking(
        [session = std::move(session), chunk = std::move(chunk)]() mutable {
            return session->send(std::move(chunk));
        });
}

} // namespace flux::ws
