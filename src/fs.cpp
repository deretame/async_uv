#include "async_uv/fs.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "async_uv/cancel.h"
#include "detail/bridge.h"
#include "detail/cancel.h"

namespace async_uv {

namespace {

constexpr uv_file kInvalidFile = static_cast<uv_file>(-1);

struct EmptyState {};

struct ReadState {
    std::string buffer;
    std::int64_t offset = -1;
};

struct WriteState {
    std::string data;
    std::int64_t offset = -1;
};

struct CopyFileState {
    std::string from;
    std::string to;
    int flags = 0;
};

struct TimedPathState {
    std::string path;
    double access_time = 0;
    double modify_time = 0;
};

struct FileTimedState {
    uv_file file = kInvalidFile;
    double access_time = 0;
    double modify_time = 0;
};

struct FileModeState {
    uv_file file = kInvalidFile;
    int mode = 0;
};

struct FileOwnerState {
    uv_file file = kInvalidFile;
    uv_uid_t uid{};
    uv_gid_t gid{};
};

struct PathOwnerState {
    std::string path;
    uv_uid_t uid{};
    uv_gid_t gid{};
};

struct SendfileState {
    uv_file out = kInvalidFile;
    uv_file in = kInvalidFile;
    std::int64_t offset = 0;
    std::size_t length = 0;
};

struct DirectoryReadState {
    uv_dir_t *dir = nullptr;
    uv_dirent_t entry{};
};

template <typename Result, typename State, typename Starter, typename Finish>
Task<Result>
run_fs_request(Runtime &runtime, const char *where, State state, Starter starter, Finish finish) {
    using StateType = std::decay_t<State>;
    using FinishType = std::decay_t<Finish>;
    auto signal = co_await get_current_signal();

    co_return co_await detail::post_task<Result>(
        runtime,
        [&runtime,
         where,
         signal = std::move(signal),
         state = StateType(std::move(state)),
         starter = std::decay_t<Starter>(std::move(starter)),
         finish = FinishType(std::move(finish))](detail::Completion<Result> complete) mutable {
            struct Op : detail::CancellableOperation<Result> {
                uv_fs_t req{};
                StateType state;
                FinishType finish;
                const char *where = nullptr;
            };

            auto op = std::make_shared<Op>();
            op->runtime = &runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->state = std::move(state);
            op->finish = std::move(finish);
            op->where = where;
            detail::attach_shared(op->req, op);

            const auto cancel_message = std::string(where) + " canceled";
            if (!detail::install_terminate_handler<Result>(op, [cancel_message](Op &op) mutable {
                    (void)uv_cancel(reinterpret_cast<uv_req_t *>(&op.req));
                    op.finish_cancel(cancel_message);
                })) {
                auto active = detail::detach_shared<Op>(&op->req);
                uv_fs_req_cleanup(&active->req);
                active->finish_cancel(cancel_message);
                return;
            }

            const int rc = starter(runtime.loop(), &op->req, op.get(), [](uv_fs_t *req) {
                auto op = detail::detach_shared<Op>(req);
                const bool already_completed = op->completed;

                if (already_completed) {
                    uv_fs_req_cleanup(req);
                    return;
                }

                if (req->result < 0) {
                    const int code = static_cast<int>(req->result);
                    uv_fs_req_cleanup(req);
                    emit_trace_event({"fs", op->where, code, 0});
                    op->finish_uv_error(op->where, code);
                    return;
                }

                try {
                    if constexpr (std::is_void_v<Result>) {
                        op->finish(*req, op->state);
                        const auto value =
                            static_cast<std::size_t>(req->result < 0 ? 0 : req->result);
                        uv_fs_req_cleanup(req);
                        emit_trace_event({"fs", op->where, 0, value});
                        op->finish_value();
                    } else {
                        auto value = op->finish(*req, op->state);
                        const auto result_size =
                            static_cast<std::size_t>(req->result < 0 ? 0 : req->result);
                        uv_fs_req_cleanup(req);
                        emit_trace_event({"fs", op->where, 0, result_size});
                        op->finish_value(std::move(value));
                    }
                } catch (...) {
                    uv_fs_req_cleanup(req);
                    emit_trace_event({"fs", op->where, UV_ECANCELED, 0});
                    op->finish_exception(std::current_exception());
                }
            });

            if (rc < 0) {
                auto active = detail::detach_shared<Op>(&op->req);
                uv_fs_req_cleanup(&active->req);
                emit_trace_event({"fs", where, rc, 0});
                active->finish_uv_error(where, rc);
            }
        });
}

template <typename Result, typename Starter, typename Finish>
Task<Result> run_fs_request(Runtime &runtime, const char *where, Starter starter, Finish finish) {
    co_return co_await run_fs_request<Result>(
        runtime, where, EmptyState{}, std::move(starter), std::move(finish));
}

FileInfo make_file_info(const uv_stat_t &stat) {
    FileInfo info;
    info.size = static_cast<std::uint64_t>(stat.st_size);
    info.mode = stat.st_mode;
    info.inode = static_cast<std::uint64_t>(stat.st_ino);
    info.device = static_cast<std::uint64_t>(stat.st_dev);
    info.uid = static_cast<int>(stat.st_uid);
    info.gid = static_cast<int>(stat.st_gid);
    info.access_time = stat.st_atim;
    info.modify_time = stat.st_mtim;
    info.change_time = stat.st_ctim;
    info.birth_time = stat.st_birthtim;
    return info;
}

FilesystemInfo make_filesystem_info(const uv_statfs_t &stat) {
    FilesystemInfo info;
    info.type = stat.f_type;
    info.block_size = stat.f_bsize;
    info.blocks = stat.f_blocks;
    info.blocks_free = stat.f_bfree;
    info.blocks_available = stat.f_bavail;
    info.files = stat.f_files;
    info.files_free = stat.f_ffree;
    info.fragment_size = stat.f_frsize;
    return info;
}

bool is_not_found_error(int code) noexcept {
    return code == UV_ENOENT || code == UV_ENOTDIR;
}

std::string join_path(const std::string &base, const std::string &child) {
    return path::join(base, child);
}

int directory_mode(int mode) noexcept {
    const int permissions = mode & 0777;
    return permissions == 0 ? 0755 : permissions;
}

} // namespace

bool FileInfo::is_file() const noexcept {
    return (mode & S_IFMT) == S_IFREG;
}

bool FileInfo::is_directory() const noexcept {
    return (mode & S_IFMT) == S_IFDIR;
}

bool FileInfo::is_symlink() const noexcept {
    return (mode & S_IFMT) == S_IFLNK;
}

bool DirectoryEntry::is_file() const noexcept {
    return type == UV_DIRENT_FILE;
}

bool DirectoryEntry::is_directory() const noexcept {
    return type == UV_DIRENT_DIR;
}

bool DirectoryEntry::is_symlink() const noexcept {
    return type == UV_DIRENT_LINK;
}

Directory::Directory(Runtime *runtime, uv_dir_t *dir) noexcept : runtime_(runtime), dir_(dir) {}

Directory::~Directory() {
    if (!is_open() || runtime_ == nullptr) {
        return;
    }

    uv_fs_t req{};
    uv_fs_closedir(runtime_->loop(), &req, dir_, nullptr);
    uv_fs_req_cleanup(&req);
    reset();
}

Directory::Directory(Directory &&other) noexcept
    : runtime_(other.runtime_), dir_(other.dir_), eof_(other.eof_) {
    other.reset();
}

Directory &Directory::operator=(Directory &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (is_open() && runtime_ != nullptr) {
        uv_fs_t req{};
        uv_fs_closedir(runtime_->loop(), &req, dir_, nullptr);
        uv_fs_req_cleanup(&req);
    }

    runtime_ = other.runtime_;
    dir_ = other.dir_;
    eof_ = other.eof_;
    other.reset();
    return *this;
}

Task<Directory> Directory::open(std::string path) {
    co_return co_await Fs::open_directory(std::move(path));
}

bool Directory::is_open() const noexcept {
    return runtime_ != nullptr && dir_ != nullptr;
}

Directory::task_type Directory::read() {
    if (!is_open()) {
        throw std::runtime_error("directory is closed");
    }

    if (eof_) {
        co_return std::nullopt;
    }

    auto *runtime = runtime_;
    auto *dir = dir_;

    auto entry = co_await run_fs_request<next_type>(
        *runtime,
        "uv_fs_readdir",
        DirectoryReadState{dir, {}},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            op->state.dir->dirents = &op->state.entry;
            op->state.dir->nentries = 1;
            return uv_fs_readdir(loop, req, op->state.dir, cb);
        },
        [](const uv_fs_t &req, DirectoryReadState &state) -> next_type {
            if (req.result == 0) {
                return std::nullopt;
            }

            return DirectoryEntry{state.entry.name, state.entry.type};
        });

    eof_ = !entry.has_value();
    co_return entry;
}

Directory::stream_type Directory::entries() {
    if (!is_open() || eof_) {
        return {};
    }

    return stream_type([this]() -> task_type {
        if (!is_open() || eof_) {
            co_return std::nullopt;
        }
        co_return co_await read();
    });
}

Task<void> Directory::close() {
    if (!is_open()) {
        co_return;
    }

    auto *runtime = runtime_;
    auto *dir = dir_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_closedir",
        dir,
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_closedir(loop, req, op->state, cb);
        },
        [](const uv_fs_t &, uv_dir_t *&) {});

    reset();
}

void Directory::reset() noexcept {
    runtime_ = nullptr;
    dir_ = nullptr;
    eof_ = false;
}

File::File(Runtime *runtime, uv_file file) noexcept : runtime_(runtime), file_(file) {}

File::~File() {
    if (!is_open() || runtime_ == nullptr) {
        return;
    }

    uv_fs_t req{};
    uv_fs_close(runtime_->loop(), &req, file_, nullptr);
    uv_fs_req_cleanup(&req);
    reset();
}

File::File(File &&other) noexcept : runtime_(other.runtime_), file_(other.file_) {
    other.reset();
}

File &File::operator=(File &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (is_open() && runtime_ != nullptr) {
        uv_fs_t req{};
        uv_fs_close(runtime_->loop(), &req, file_, nullptr);
        uv_fs_req_cleanup(&req);
    }

    runtime_ = other.runtime_;
    file_ = other.file_;
    other.reset();
    return *this;
}

bool File::is_open() const noexcept {
    return runtime_ != nullptr && file_ != kInvalidFile;
}

void File::reset() noexcept {
    runtime_ = nullptr;
    file_ = kInvalidFile;
}

Task<File> File::open(std::string path, int flags, int mode) {
    co_return co_await Fs::open(std::move(path), flags, mode);
}

Task<File> File::open(std::string path, OpenFlags flags, int mode) {
    co_return co_await Fs::open(std::move(path), flags, mode);
}

Task<std::string> File::read_some(std::size_t max_bytes) {
    co_return co_await read_some_at(-1, max_bytes);
}

Task<std::string> File::read_some_at(std::int64_t offset, std::size_t max_bytes) {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    const auto read_size = std::max<std::size_t>(max_bytes, 1);
    auto *runtime = runtime_;
    const uv_file file = file_;

    co_return co_await run_fs_request<std::string>(
        *runtime,
        "uv_fs_read",
        ReadState{std::string(read_size, '\0'), offset},
        [file](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            uv_buf_t buf = uv_buf_init(op->state.buffer.data(),
                                       static_cast<unsigned int>(op->state.buffer.size()));
            return uv_fs_read(loop, req, file, &buf, 1, op->state.offset, cb);
        },
        [](const uv_fs_t &req, ReadState &state) {
            state.buffer.resize(static_cast<std::size_t>(req.result));
            return std::move(state.buffer);
        });
}

Task<std::string> File::read_all(std::size_t chunk_size) {
    std::string result;

    for (;;) {
        co_await throw_if_cancelled("uv_fs_read canceled");
        auto chunk = co_await read_some(chunk_size);
        if (chunk.empty()) {
            break;
        }
        result += chunk;
    }

    co_return result;
}

Task<std::size_t> File::write_some(std::string_view data) {
    co_return co_await write_some_at(-1, data);
}

Task<std::size_t> File::write_some_at(std::int64_t offset, std::string_view data) {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    if (data.empty()) {
        co_return 0;
    }

    auto *runtime = runtime_;
    const uv_file file = file_;

    co_return co_await run_fs_request<std::size_t>(
        *runtime,
        "uv_fs_write",
        WriteState{std::string(data), offset},
        [file](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            uv_buf_t buf = uv_buf_init(op->state.data.data(),
                                       static_cast<unsigned int>(op->state.data.size()));
            return uv_fs_write(loop, req, file, &buf, 1, op->state.offset, cb);
        },
        [](const uv_fs_t &req, WriteState &) {
            return static_cast<std::size_t>(req.result);
        });
}

Task<std::size_t> File::write_all(std::string_view data) {
    co_return co_await write_all_at(-1, data);
}

Task<std::size_t> File::write_all_at(std::int64_t offset, std::string_view data) {
    std::size_t written = 0;
    auto current_offset = offset;

    while (written < data.size()) {
        co_await throw_if_cancelled("uv_fs_write canceled");
        const auto chunk = co_await write_some_at(current_offset, data.substr(written));
        if (chunk == 0) {
            throw std::runtime_error("uv_fs_write returned 0 before all data was written");
        }
        written += chunk;
        if (current_offset >= 0) {
            current_offset += static_cast<std::int64_t>(chunk);
        }
    }

    co_return written;
}

Task<FileInfo> File::stat() {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;
    const uv_file file = file_;

    co_return co_await run_fs_request<FileInfo>(
        *runtime,
        "uv_fs_fstat",
        [file](uv_loop_t *loop, uv_fs_t *req, auto *, uv_fs_cb cb) {
            return uv_fs_fstat(loop, req, file, cb);
        },
        [](const uv_fs_t &req, EmptyState &) {
            return make_file_info(req.statbuf);
        });
}

Task<void> File::truncate(std::uint64_t size) {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;
    const uv_file file = file_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_ftruncate",
        std::pair<uv_file, std::int64_t>(file, static_cast<std::int64_t>(size)),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_ftruncate(loop, req, op->state.first, op->state.second, cb);
        },
        [](const uv_fs_t &, std::pair<uv_file, std::int64_t> &) {});
}

Task<void> File::sync() {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;
    const uv_file file = file_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_fsync",
        file,
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_fsync(loop, req, op->state, cb);
        },
        [](const uv_fs_t &, uv_file &) {});
}

Task<void> File::datasync() {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;
    const uv_file file = file_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_fdatasync",
        file,
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_fdatasync(loop, req, op->state, cb);
        },
        [](const uv_fs_t &, uv_file &) {});
}

Task<void> File::chmod(int mode) {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_fchmod",
        FileModeState{file_, mode},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_fchmod(loop, req, op->state.file, op->state.mode, cb);
        },
        [](const uv_fs_t &, FileModeState &) {});
}

Task<void> File::chown(uv_uid_t uid, uv_gid_t gid) {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_fchown",
        FileOwnerState{file_, uid, gid},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_fchown(loop, req, op->state.file, op->state.uid, op->state.gid, cb);
        },
        [](const uv_fs_t &, FileOwnerState &) {});
}

Task<void> File::utime(double access_time, double modify_time) {
    if (!is_open()) {
        throw std::runtime_error("file is closed");
    }

    auto *runtime = runtime_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_futime",
        FileTimedState{file_, access_time, modify_time},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_futime(
                loop, req, op->state.file, op->state.access_time, op->state.modify_time, cb);
        },
        [](const uv_fs_t &, FileTimedState &) {});
}

Task<std::size_t> File::send_to(File &destination, std::uint64_t length, std::int64_t offset) {
    if (!is_open()) {
        throw std::runtime_error("source file is closed");
    }
    if (!destination.is_open()) {
        throw std::runtime_error("destination file is closed");
    }
    if (length == 0) {
        co_return 0;
    }

    auto *runtime = runtime_;

    co_return co_await run_fs_request<std::size_t>(
        *runtime,
        "uv_fs_sendfile",
        SendfileState{destination.file_, file_, offset, static_cast<std::size_t>(length)},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_sendfile(
                loop, req, op->state.out, op->state.in, op->state.offset, op->state.length, cb);
        },
        [](const uv_fs_t &req, SendfileState &) {
            return static_cast<std::size_t>(req.result);
        });
}

Task<void> File::close() {
    if (!is_open()) {
        co_return;
    }

    auto *runtime = runtime_;
    const uv_file file = file_;

    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_close",
        [file](uv_loop_t *loop, uv_fs_t *req, auto *, uv_fs_cb cb) {
            return uv_fs_close(loop, req, file, cb);
        },
        [](const uv_fs_t &, EmptyState &) {});

    reset();
}

Task<Directory> Fs::open_directory(std::string path) {
    auto *runtime = co_await get_current_runtime();

    auto *dir = co_await run_fs_request<uv_dir_t *>(
        *runtime,
        "uv_fs_opendir",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_opendir(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            return static_cast<uv_dir_t *>(req.ptr);
        });

    co_return Directory(runtime, dir);
}

Task<File> Fs::open(std::string path, int flags, int mode) {
    auto *runtime = co_await get_current_runtime();

    const uv_file file = co_await run_fs_request<uv_file>(
        *runtime,
        "uv_fs_open",
        std::move(path),
        [flags, mode](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_open(loop, req, op->state.c_str(), flags, mode, cb);
        },
        [](const uv_fs_t &req, std::string &) {
            return static_cast<uv_file>(req.result);
        });

    co_return File(runtime, file);
}

Task<File> Fs::open(std::string path, OpenFlags flags, int mode) {
    co_return co_await open(std::move(path), std::to_underlying(flags), mode);
}

Task<std::string> Fs::read_file(std::string path) {
    auto file = co_await open(std::move(path), O_RDONLY);
    auto content = co_await file.read_all();
    co_await file.close();
    co_return content;
}

Task<void> Fs::write_file(std::string path, std::string_view content, int mode) {
    auto file = co_await open(std::move(path), O_WRONLY | O_CREAT | O_TRUNC, mode);
    co_await file.write_all(content);
    co_await file.close();
}

Task<void> Fs::append_file(std::string path, std::string_view content, int mode) {
    auto file = co_await open(std::move(path), O_WRONLY | O_CREAT | O_APPEND, mode);
    co_await file.write_all(content);
    co_await file.close();
}

Task<void> Fs::create_file(std::string path, int mode) {
    auto file = co_await open(std::move(path), O_WRONLY | O_CREAT | O_TRUNC, mode);
    co_await file.close();
}

Task<void> Fs::touch(std::string path, int mode) {
    auto file = co_await open(std::move(path), O_WRONLY | O_CREAT, mode);
    co_await file.close();
}

Task<bool> Fs::access(std::string path, int mode) {
    auto *runtime = co_await get_current_runtime();
    auto signal = co_await get_current_signal();

    co_return co_await detail::post_task<bool>(
        *runtime,
        [runtime, signal = std::move(signal), path = std::move(path), mode](
            detail::Completion<bool> complete) mutable {
            struct Op : detail::CancellableOperation<bool> {
                uv_fs_t req{};
                std::string path;
                int mode = 0;
            };

            auto op = std::make_shared<Op>();
            op->runtime = runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->path = std::move(path);
            op->mode = mode;
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_fs_access canceled";
            if (!detail::install_terminate_handler<bool>(op, [](Op &op) {
                    (void)uv_cancel(reinterpret_cast<uv_req_t *>(&op.req));
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<Op>(&op->req);
                uv_fs_req_cleanup(&active->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            const int rc = uv_fs_access(
                runtime->loop(), &op->req, op->path.c_str(), op->mode, [](uv_fs_t *req) {
                    auto op = detail::detach_shared<Op>(req);
                    const bool already_completed = op->completed;
                    if (already_completed) {
                        uv_fs_req_cleanup(req);
                        return;
                    }

                    if (req->result < 0) {
                        const int code = static_cast<int>(req->result);
                        if (is_not_found_error(code) || code == UV_EACCES || code == UV_EPERM) {
                            uv_fs_req_cleanup(req);
                            op->finish_value(false);
                        } else {
                            uv_fs_req_cleanup(req);
                            op->finish_uv_error("uv_fs_access", code);
                        }
                    } else {
                        uv_fs_req_cleanup(req);
                        op->finish_value(true);
                    }
                });

            if (rc < 0) {
                auto active = detail::detach_shared<Op>(&op->req);
                uv_fs_req_cleanup(&active->req);
                active->finish_uv_error("uv_fs_access", rc);
            }
        });
}

Task<bool> Fs::access(std::string path, AccessFlags mode) {
    co_return co_await access(std::move(path), std::to_underlying(mode));
}

Task<bool> Fs::exists(std::string path) {
    co_return co_await access(std::move(path), 0);
}

Task<FileInfo> Fs::stat(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<FileInfo>(
        *runtime,
        "uv_fs_stat",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_stat(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            return make_file_info(req.statbuf);
        });
}

Task<FileInfo> Fs::lstat(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<FileInfo>(
        *runtime,
        "uv_fs_lstat",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_lstat(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            return make_file_info(req.statbuf);
        });
}

Task<std::vector<DirectoryEntry>> Fs::list_directory(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<std::vector<DirectoryEntry>>(
        *runtime,
        "uv_fs_scandir",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_scandir(loop, req, op->state.c_str(), 0, cb);
        },
        [](const uv_fs_t &req, std::string &) {
            std::vector<DirectoryEntry> entries;
            entries.reserve(static_cast<std::size_t>(req.result));

            uv_dirent_t entry{};
            for (;;) {
                const int rc = uv_fs_scandir_next(const_cast<uv_fs_t *>(&req), &entry);
                if (rc == UV_EOF) {
                    break;
                }
                if (rc < 0) {
                    throw Error("uv_fs_scandir_next", rc);
                }

                entries.push_back(DirectoryEntry{entry.name, entry.type});
            }

            return entries;
        });
}

Task<void> Fs::create_directory(std::string path, int mode) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_mkdir",
        std::move(path),
        [mode](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_mkdir(loop, req, op->state.c_str(), mode, cb);
        },
        [](const uv_fs_t &, std::string &) {});
}

Task<void> Fs::create_directories(std::string path, int mode) {
    if (path.empty()) {
        co_return;
    }

    auto full_path = std::filesystem::path(std::move(path)).lexically_normal();
    if (full_path.empty()) {
        co_return;
    }

    std::filesystem::path current = full_path.root_path();
    for (const auto &part : full_path.relative_path()) {
        co_await throw_if_cancelled("uv_fs_mkdir canceled");
        if (part.empty() || part == ".") {
            continue;
        }

        current /= part;
        if (part == "..") {
            continue;
        }

        const auto current_path = current.string();
        if (co_await exists(current_path)) {
            auto info = co_await stat(current_path);
            if (!info.is_directory()) {
                throw std::runtime_error("path component is not a directory: " + current_path);
            }
            continue;
        }

        co_await create_directory(current_path, mode);
    }
}

Task<void> Fs::remove(std::string path) {
    const auto info = co_await lstat(path);
    if (info.is_directory() && !info.is_symlink()) {
        co_await remove_directory(std::move(path));
        co_return;
    }

    co_await remove_file(std::move(path));
}

Task<void> Fs::remove_file(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_unlink",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_unlink(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &, std::string &) {});
}

Task<void> Fs::remove_directory(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_rmdir",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_rmdir(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &, std::string &) {});
}

Task<std::size_t> Fs::remove_all(std::string path) {
    co_await throw_if_cancelled("uv_fs_remove_all canceled");

    if (!co_await exists(path)) {
        co_return 0;
    }

    const auto info = co_await lstat(path);
    if (info.is_directory() && !info.is_symlink()) {
        std::size_t removed = 0;
        for (const auto &entry : co_await list_directory(path)) {
            co_await throw_if_cancelled("uv_fs_remove_all canceled");
            removed += co_await remove_all(join_path(path, entry.name));
        }
        co_await remove_directory(path);
        co_return removed + 1;
    }

    co_await remove_file(std::move(path));
    co_return 1;
}

Task<void> Fs::copy_file(std::string from, std::string to, int flags) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_copyfile",
        CopyFileState{std::move(from), std::move(to), flags},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_copyfile(
                loop, req, op->state.from.c_str(), op->state.to.c_str(), op->state.flags, cb);
        },
        [](const uv_fs_t &, CopyFileState &) {});
}

Task<void> Fs::copy_file(std::string from, std::string to, CopyFlags flags) {
    co_await copy_file(std::move(from), std::move(to), std::to_underlying(flags));
}

Task<void> Fs::copy(std::string from, std::string to, int copy_file_flags) {
    co_await throw_if_cancelled("uv_fs_copy canceled");

    const auto source_info = co_await lstat(from);

    if (source_info.is_symlink()) {
        auto target = co_await read_link(from);
        int flags = 0;
#if defined(_WIN32)
        try {
            if ((co_await stat(from)).is_directory()) {
                flags |= UV_FS_SYMLINK_DIR;
            }
        } catch (...) {
        }
#endif
        co_await symlink(std::move(target), std::move(to), flags);
        co_return;
    }

    if (source_info.is_directory()) {
        if (co_await exists(to)) {
            const auto destination_info = co_await stat(to);
            if (!destination_info.is_directory()) {
                throw std::runtime_error("copy destination exists and is not a directory: " + to);
            }
        } else {
            co_await create_directories(to, directory_mode(source_info.mode));
        }

        for (const auto &entry : co_await list_directory(from)) {
            co_await throw_if_cancelled("uv_fs_copy canceled");
            co_await copy(join_path(from, entry.name), join_path(to, entry.name), copy_file_flags);
        }
        co_return;
    }

    co_await copy_file(std::move(from), std::move(to), copy_file_flags);
}

Task<void> Fs::copy(std::string from, std::string to, CopyFlags copy_file_flags) {
    co_await copy(std::move(from), std::move(to), std::to_underlying(copy_file_flags));
}

Task<void> Fs::move(std::string from, std::string to) {
    try {
        co_await rename(from, to);
        co_return;
    } catch (const Error &error) {
        if (error.code() != UV_EXDEV) {
            throw;
        }
    }

    const auto source_info = co_await lstat(from);
    co_await copy(from, to);

    if (source_info.is_directory() && !source_info.is_symlink()) {
        (void)co_await remove_all(std::move(from));
        co_return;
    }

    co_await remove_file(std::move(from));
}

Task<void> Fs::rename(std::string from, std::string to) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_rename",
        std::pair<std::string, std::string>(std::move(from), std::move(to)),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_rename(loop, req, op->state.first.c_str(), op->state.second.c_str(), cb);
        },
        [](const uv_fs_t &, std::pair<std::string, std::string> &) {});
}

Task<void> Fs::link(std::string from, std::string to) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_link",
        std::pair<std::string, std::string>(std::move(from), std::move(to)),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_link(loop, req, op->state.first.c_str(), op->state.second.c_str(), cb);
        },
        [](const uv_fs_t &, std::pair<std::string, std::string> &) {});
}

Task<void> Fs::symlink(std::string target, std::string path, int flags) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_symlink",
        CopyFileState{std::move(target), std::move(path), flags},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_symlink(
                loop, req, op->state.from.c_str(), op->state.to.c_str(), op->state.flags, cb);
        },
        [](const uv_fs_t &, CopyFileState &) {});
}

Task<void> Fs::symlink(std::string target, std::string path, SymlinkFlags flags) {
    co_await symlink(std::move(target), std::move(path), std::to_underlying(flags));
}

Task<std::string> Fs::read_link(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<std::string>(
        *runtime,
        "uv_fs_readlink",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_readlink(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            const auto *link = static_cast<const char *>(req.ptr);
            return std::string(link == nullptr ? "" : link);
        });
}

Task<std::string> Fs::real_path(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<std::string>(
        *runtime,
        "uv_fs_realpath",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_realpath(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            const auto *resolved = static_cast<const char *>(req.ptr);
            return std::string(resolved == nullptr ? "" : resolved);
        });
}

Task<void> Fs::chmod(std::string path, int mode) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_chmod",
        std::pair<std::string, int>(std::move(path), mode),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_chmod(loop, req, op->state.first.c_str(), op->state.second, cb);
        },
        [](const uv_fs_t &, std::pair<std::string, int> &) {});
}

Task<void> Fs::chown(std::string path, uv_uid_t uid, uv_gid_t gid) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_chown",
        PathOwnerState{std::move(path), uid, gid},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_chown(loop, req, op->state.path.c_str(), op->state.uid, op->state.gid, cb);
        },
        [](const uv_fs_t &, PathOwnerState &) {});
}

Task<void> Fs::lchown(std::string path, uv_uid_t uid, uv_gid_t gid) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_lchown",
        PathOwnerState{std::move(path), uid, gid},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_lchown(
                loop, req, op->state.path.c_str(), op->state.uid, op->state.gid, cb);
        },
        [](const uv_fs_t &, PathOwnerState &) {});
}

Task<void> Fs::utime(std::string path, double access_time, double modify_time) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_utime",
        TimedPathState{std::move(path), access_time, modify_time},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_utime(loop,
                               req,
                               op->state.path.c_str(),
                               op->state.access_time,
                               op->state.modify_time,
                               cb);
        },
        [](const uv_fs_t &, TimedPathState &) {});
}

Task<void> Fs::lutime(std::string path, double access_time, double modify_time) {
    auto *runtime = co_await get_current_runtime();
    co_await run_fs_request<void>(
        *runtime,
        "uv_fs_lutime",
        TimedPathState{std::move(path), access_time, modify_time},
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_lutime(loop,
                                req,
                                op->state.path.c_str(),
                                op->state.access_time,
                                op->state.modify_time,
                                cb);
        },
        [](const uv_fs_t &, TimedPathState &) {});
}

Task<FilesystemInfo> Fs::statfs(std::string path) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<FilesystemInfo>(
        *runtime,
        "uv_fs_statfs",
        std::move(path),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_statfs(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            const auto *stat = static_cast<const uv_statfs_t *>(req.ptr);
            if (stat == nullptr) {
                throw std::runtime_error("uv_fs_statfs returned no filesystem data");
            }
            return make_filesystem_info(*stat);
        });
}

Task<std::string> Fs::create_temporary_directory(std::string path_template) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await run_fs_request<std::string>(
        *runtime,
        "uv_fs_mkdtemp",
        std::move(path_template),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_mkdtemp(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            return std::string(req.path == nullptr ? "" : req.path);
        });
}

Task<TempFile> Fs::create_temporary_file(std::string path_template) {
    auto *runtime = co_await get_current_runtime();
    auto [path, file] = co_await run_fs_request<std::pair<std::string, uv_file>>(
        *runtime,
        "uv_fs_mkstemp",
        std::move(path_template),
        [](uv_loop_t *loop, uv_fs_t *req, auto *op, uv_fs_cb cb) {
            return uv_fs_mkstemp(loop, req, op->state.c_str(), cb);
        },
        [](const uv_fs_t &req, std::string &) {
            return std::pair<std::string, uv_file>(std::string(req.path == nullptr ? "" : req.path),
                                                   static_cast<uv_file>(req.result));
        });

    co_return TempFile{File(runtime, file), std::move(path)};
}

Task<std::string> read_file(std::string path) {
    co_return co_await Fs::read_file(std::move(path));
}

Task<void> write_file(std::string path, std::string_view content, int mode) {
    co_await Fs::write_file(std::move(path), content, mode);
}

} // namespace async_uv
