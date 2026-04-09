#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "flux/runtime.h"
#include "flux_layer2/error.h"
#include "flux_layer2/to_string.h"
#include "flux_ws/ws.h"

namespace {

constexpr const char *kCertPem = R"PEM(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUULEwMmnD65Or4VF7MdEFWk6iNoMwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDMyODEwMTAxNFoXDTI3MDMy
ODEwMTAxNFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA0h5PzfPEky62Eqsa7Csv28KOoZ+I+gaONOT4XOz0EDSG
NpwdkuP8JKeP2j5Bh7bssqg49e2zj9SbzVcKfGuk5hj0vqc8YlfFmiKEQl6gGQko
zvcmHv6n/5VryhFVmbCji/7GiPc91beaPb/UqdkfqvoHBkGuNNfxCLPoNXE/U6wd
2s8MlyM7uRX56U5UZ2GskMHXmlMGM5oYI0aFA8deOrqg/yYgfj9rJBnbJRmYgviC
sn9NY5Ny6Qh6gDKJBi4yQrw6wZWiCEzIUrDMkxtDIFC4dwnp601oO+j++DBRN9+t
ZAiYYjk8dh3ZEYtgmkdU5SbPZwsk66tXF8aPbBF2jwIDAQABo1MwUTAdBgNVHQ4E
FgQUfaLtKdcr+BoL5MywYEoBO2B/XFkwHwYDVR0jBBgwFoAUfaLtKdcr+BoL5Myw
YEoBO2B/XFkwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEANuUi
pgVxLTuU+5lSutnoo4867moMYZFbEIRWi8rGOMhxg7Ha800KcEQZFaI4Q8xxGFUe
fs/XdpItxDjlppbX1NPT6tRTTa7r9htj3m7XhvwIFuTjtUVETI7ygR6kdABKfkRE
NJ1x5Ib2AQsrrlzdoGNlVTr/jhs59AR5sqKCp4iHdRu6hKnJQtMiO3h/MFoNvM+D
P406gfKX/9yByzycrvzQzRSGloC4Tfo13xo53hwVvUmKABV3Ter05EKci8iybBv0
oac87ZNAzgAM7JxDZtunli+sigh1A+X5rmM7pQjEDXH074FnQ2O7aJA17zITXK89
VbAdL0HxcNFF9rlSqA==
-----END CERTIFICATE-----
)PEM";

constexpr const char *kKeyPem = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDSHk/N88STLrYS
qxrsKy/bwo6hn4j6Bo405Phc7PQQNIY2nB2S4/wkp4/aPkGHtuyyqDj17bOP1JvN
Vwp8a6TmGPS+pzxiV8WaIoRCXqAZCSjO9yYe/qf/lWvKEVWZsKOL/saI9z3Vt5o9
v9Sp2R+q+gcGQa401/EIs+g1cT9TrB3azwyXIzu5FfnpTlRnYayQwdeaUwYzmhgj
RoUDx146uqD/JiB+P2skGdslGZiC+IKyf01jk3LpCHqAMokGLjJCvDrBlaIITMhS
sMyTG0MgULh3CenrTWg76P74MFE3361kCJhiOTx2HdkRi2CaR1TlJs9nCyTrq1cX
xo9sEXaPAgMBAAECggEACcTdAv9NaWZnkrCf3NEZNYdzKDKJpM4/OFhY78EYi9RV
VR6nBVhSNcYQmx3/3/ZyRNArbcuyaIKDB+X/F8/NZ+FLJ74QwaXBCyyFp6xMlz1k
x1KYVCKU3v4sd8WcjDu5Lt4WjvGnz1Ls+ef8VDVDEqjzQKg3GD/d9gCfUDTGs/Yb
+vWOFXtGjp6cKSLHNhTZdA2Kg4/YlTixnCeennimF4DQrh21ww3bZIVsydYaR01F
rdvPxU3pjk0JQJnH0Wi5NXFG1eZQHrkFPLqDVkDF5QH0xNmmEO0FmZb9jp4n4QKB
gjX2jnCGKPs2SVJs7EhbsxW/HPd6EggJ5DroZgkEiQKBgQD9OMLso39N5JO8I4wW
4kg2gCBwxQhWhYh8FmMk596OgdoK9b0sUWntTuH8gHd3lh6IBuL+cUfhvWP6TvKS
f2855NSs9sjY0yt6raIojHUUt1jkwDdnTuYuVy5Cvi3bgSEXnLJHJZWjU058x516
6wbO292knjfzej4IashzCI+ArQKBgQDUbHvHpY7r6BIY04rtjCbb5t/c3lXZLcBh
FUVGOWB9CV+av1uweiljFWwvspbcVVKLlrutijmD4LvPnngino+zX/bSVdXxMliB
gR5uMNEEgQJukul0Mh0/OBvYvcXB/MhJmVcX0cM45NuwQs/h7acxpVofNtZ3eHmq
EFFRqQnvqwKBgQDOluyQ26MVDZNqPyYf1WVM8aOF3Xo7/J2pfypMBdARO+eEYZCB
A7sEHQNKWhUdv6ARIm03YXxfs4BJyvckhktcVFEe/AhIvaAPanGN22n6CMvBdQC+
jCRHUmEvmrEXEHbLKNBaM1Ot+F1keAcHLZBUXBSsJVlIj0bk3xnCoA1T0QKBgHra
rw/9WVZopqbDGfNe/k5qDYjA8eekRUIguiruHjbSh/+Isq+zR2Jtzl8bq5KMqivf
JnYsniz+ecCPBy4GhFeapbZqPEy98GAd3AqgoxI2xsBKqUgxf6bDfZ9xygDygKfI
To2RHJY4DjK3wWEKQIs+9Ytd/NWl0L+hplZTLFL3AoGBAIA4F6Rk5fzVBTvStw4j
w7SnlbOxc5SOqAcD+aDu7rQ5lAKqXaY1yWzSicD/kmFp2/wEv4TVmBKuWoxYI8ty
fo83aEI1D+2VG9rjvJ2sFYWco3Bz1Hs81mD1I/6vslvN8mYUUB8wGYRQAQ2IP1zV
WGf73tZzw5Rs9DKk6JyVH//C
-----END PRIVATE KEY-----
)PEM";

void write_text_file(const std::filesystem::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    assert(out.good());
}

flux::Task<void> run_plain_ws(flux::Runtime &runtime) {
    auto server = flux::ws::Server::create({
        .runtime = &runtime,
        .host = "127.0.0.1",
        .port = 0,
    });

    co_await server.start();
    assert(server.started());
    assert(server.port() > 0);

    auto client = flux::ws::Client::create({
        .runtime = &runtime,
        .address = "127.0.0.1",
        .port = server.port(),
        .path = "/",
    });

    co_await client.connect();
    assert(client.connected());

    auto server_open = co_await server.next();
    assert(server_open.has_value());
    assert(server_open->kind == flux::ws::ServerEvent::Kind::open);
    const auto connection_id = server_open->connection_id;

    auto client_open = co_await client.next();
    assert(client_open.has_value());
    assert(client_open->kind == flux::ws::Client::Event::Kind::open);

    assert(flux::layer2::to_string(flux::ws::MessageType::text) == "text");

    {
        flux::ws::StreamChunk chunk;
        chunk.type = flux::ws::MessageType::text;
        chunk.payload = "hel";
        chunk.is_first_fragment = true;
        chunk.is_final_fragment = false;
        const bool sent = co_await client.send_stream(std::move(chunk));
        assert(sent);
    }
    {
        flux::ws::StreamChunk chunk;
        chunk.type = flux::ws::MessageType::text;
        chunk.payload = "lo";
        chunk.is_first_fragment = false;
        chunk.is_final_fragment = true;
        const bool sent = co_await client.send_stream(std::move(chunk));
        assert(sent);
    }

    auto server_msg_1 = co_await server.next();
    assert(server_msg_1.has_value());
    assert(server_msg_1->kind == flux::ws::ServerEvent::Kind::message);
    assert(server_msg_1->connection_id == connection_id);
    assert(server_msg_1->message.has_value());
    assert(server_msg_1->message->payload == "hel");
    assert(!server_msg_1->message->is_final_fragment);

    auto server_msg_2 = co_await server.next();
    assert(server_msg_2.has_value());
    assert(server_msg_2->kind == flux::ws::ServerEvent::Kind::message);
    assert(server_msg_2->connection_id == connection_id);
    assert(server_msg_2->message.has_value());
    assert(server_msg_2->message->payload == "lo");
    assert(server_msg_2->message->is_final_fragment);

    {
        flux::ws::StreamChunk chunk;
        chunk.type = flux::ws::MessageType::binary;
        chunk.payload = "12";
        chunk.is_first_fragment = true;
        chunk.is_final_fragment = false;
        const bool sent = co_await server.send_stream(connection_id, std::move(chunk));
        assert(sent);
    }
    {
        flux::ws::StreamChunk chunk;
        chunk.type = flux::ws::MessageType::binary;
        chunk.payload = "34";
        chunk.is_first_fragment = false;
        chunk.is_final_fragment = true;
        const bool sent = co_await server.send_stream(connection_id, std::move(chunk));
        assert(sent);
    }

    auto client_msg_1 = co_await client.next();
    assert(client_msg_1.has_value());
    assert(client_msg_1->kind == flux::ws::Client::Event::Kind::message);
    assert(client_msg_1->message.has_value());
    assert(client_msg_1->message->payload == "12");

    auto client_msg_2 = co_await client.next();
    assert(client_msg_2.has_value());
    assert(client_msg_2->kind == flux::ws::Client::Event::Kind::message);
    assert(client_msg_2->message.has_value());
    assert(client_msg_2->message->payload == "34");

    const auto text = flux::layer2::to_string(*client_msg_2->message);
    assert(text.find("type=binary") != std::string::npos);
    assert(text.find("payload_size=2") != std::string::npos);

    co_await client.close(1000, "done");
    auto server_close = co_await server.next();
    assert(server_close.has_value());
    assert(server_close->kind == flux::ws::ServerEvent::Kind::close);

    {
        flux::ws::StreamChunk chunk;
        chunk.type = flux::ws::MessageType::binary;
        chunk.payload = "payload";
        chunk.is_first_fragment = true;
        chunk.is_final_fragment = false;
        const bool sent_server = co_await server.send_stream(connection_id, std::move(chunk));
        assert(!sent_server);
    }

    co_await server.stop();
    assert(!server.started());
}

flux::Task<void> run_wss(flux::Runtime &runtime) {
    const auto temp = std::filesystem::temp_directory_path() / "flux_wss_test";
    std::filesystem::create_directories(temp);
    const auto cert_path = temp / "server.crt";
    const auto key_path = temp / "server.key";
    write_text_file(cert_path, kCertPem);
    write_text_file(key_path, kKeyPem);

    auto server = flux::ws::Server::create({
        .runtime = &runtime,
        .host = "127.0.0.1",
        .port = 0,
        .use_tls = true,
        .tls_cert_file = cert_path.string(),
        .tls_private_key_file = key_path.string(),
    });
    co_await server.start();
    assert(server.started());
    assert(server.port() > 0);

    auto client = flux::ws::Client::create({
        .runtime = &runtime,
        .address = "127.0.0.1",
        .port = server.port(),
        .path = "/",
        .host = "localhost",
        .use_tls = true,
        .tls_allow_insecure = true,
    });

    co_await client.connect();
    auto server_open = co_await server.next();
    assert(server_open.has_value());
    assert(server_open->kind == flux::ws::ServerEvent::Kind::open);
    const auto connection_id = server_open->connection_id;

    auto client_open = co_await client.next();
    assert(client_open.has_value());
    assert(client_open->kind == flux::ws::Client::Event::Kind::open);

    {
        const bool sent = co_await client.send_text("wss-c2s");
        assert(sent);
    }
    auto server_msg = co_await server.next();
    assert(server_msg.has_value());
    assert(server_msg->kind == flux::ws::ServerEvent::Kind::message);
    assert(server_msg->connection_id == connection_id);
    assert(server_msg->message.has_value());
    assert(server_msg->message->payload == "wss-c2s");

    {
        const bool sent = co_await server.send_binary(connection_id, "wss-s2c");
        assert(sent);
    }
    auto client_msg = co_await client.next();
    assert(client_msg.has_value());
    assert(client_msg->kind == flux::ws::Client::Event::Kind::message);
    assert(client_msg->message.has_value());
    assert(client_msg->message->payload == "wss-s2c");

    co_await client.close(1000, "done");
    co_await server.stop();
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().name("flux_ws_test"));
    runtime.block_on(run_plain_ws(runtime));
    runtime.block_on(run_wss(runtime));
    return 0;
}
