#include "flux/fs.h"

#include <cerrno>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace flux {

namespace {

template <typename Func>
Task<BlockingValue<Func>> run_blocking(Func &&func) {
    auto *runtime = co_await get_current_runtime();
    using Work = std::decay_t<Func>;
    using ValueType = BlockingValue<Func>;

    auto work = Work(std::forward<Func>(func));
    auto sender = stdexec::starts_on(
        runtime->blocking_scheduler(),
        stdexec::just() | stdexec::then([work = std::move(work)]() mutable -> ValueType {
            if constexpr (std::is_void_v<ValueType>) {
                std::invoke(work);
            } else {
                return std::invoke(work);
            }
        }));
    co_return co_await std::move(sender);
}

FileInfo to_file_info(const std::filesystem::path &path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        throw std::system_error(ec);
    }

    FileInfo info;
    info.regular_file = std::filesystem::is_regular_file(status);
    info.directory = std::filesystem::is_directory(status);
    info.symlink = std::filesystem::is_symlink(status);
    if (info.regular_file) {
        info.size = std::filesystem::file_size(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
    }
    info.last_write_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        throw std::system_error(ec);
    }
    return info;
}

std::string ensure_tmp_pattern(const std::string &pattern, const char *fallback) {
    std::string value = pattern.empty() ? std::string(fallback) : pattern;
    if (value.find("XXXXXX") == std::string::npos) {
        if (!value.empty() && value.back() != '-') {
            value += "-";
        }
        value += "XXXXXX";
    }
    return value;
}

std::string make_tmp_template_path(const std::string &pattern, const char *fallback) {
    const auto tmp_dir = std::filesystem::temp_directory_path();
    return (tmp_dir / ensure_tmp_pattern(pattern, fallback)).string();
}

bool has_flag(OpenFlags value, OpenFlags flag) {
    return (value & flag) == flag;
}

int to_posix_open_flags(OpenFlags flags) {
    int native = 0;

    if (has_flag(flags, OpenFlags::read_write)) {
        native |= O_RDWR;
    } else if (has_flag(flags, OpenFlags::write_only)) {
        native |= O_WRONLY;
    } else {
        native |= O_RDONLY;
    }

    if (has_flag(flags, OpenFlags::create)) {
        native |= O_CREAT;
    }
    if (has_flag(flags, OpenFlags::append)) {
        native |= O_APPEND;
    }
    if (has_flag(flags, OpenFlags::truncate)) {
        native |= O_TRUNC;
    }
    return native;
}

} // namespace

Task<bool> Fs::exists(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        const bool value = std::filesystem::exists(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return value;
    });
}

Task<bool> Fs::is_file(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        const bool value = std::filesystem::is_regular_file(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return value;
    });
}

Task<bool> Fs::is_directory(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        const bool value = std::filesystem::is_directory(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return value;
    });
}

Task<FileInfo> Fs::stat(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] {
        return to_file_info(path);
    });
}

Task<std::uint64_t> Fs::file_size(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] -> std::uint64_t {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return static_cast<std::uint64_t>(size);
    });
}

Task<std::vector<DirectoryEntry>> Fs::list_directory(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] {
        std::vector<DirectoryEntry> entries;
        for (const auto &entry : std::filesystem::directory_iterator(path)) {
            DirectoryEntry item;
            item.name = entry.path().filename().string();
            item.path = entry.path().string();
            std::error_code ec;
            const auto status = entry.symlink_status(ec);
            if (ec) {
                throw std::system_error(ec);
            }
            item.regular_file = std::filesystem::is_regular_file(status);
            item.directory = std::filesystem::is_directory(status);
            item.symlink = std::filesystem::is_symlink(status);
            entries.push_back(std::move(item));
        }
        return entries;
    });
}

Task<void> Fs::create_directories(std::filesystem::path path) {
    co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        (void)std::filesystem::create_directories(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
    });
}

Task<void> Fs::create_directory(std::filesystem::path path) {
    co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        (void)std::filesystem::create_directory(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
    });
}

Task<void> Fs::create_file(std::filesystem::path path) {
    co_await run_blocking([path = std::move(path)] {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to create file: " + path.string());
        }
    });
}

Task<std::string> Fs::read_file(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            throw std::runtime_error("failed to open file for reading: " + path.string());
        }

        const auto end = in.tellg();
        if (end < 0) {
            throw std::runtime_error("failed to query file size: " + path.string());
        }

        std::string content(static_cast<std::size_t>(end), '\0');
        in.seekg(0, std::ios::beg);
        if (!content.empty()) {
            in.read(content.data(), static_cast<std::streamsize>(content.size()));
            if (!in) {
                throw std::runtime_error("failed to read file: " + path.string());
            }
        }
        return content;
    });
}

Task<void> Fs::write_file(std::filesystem::path path, std::string_view data) {
    co_await run_blocking([path = std::move(path), data] {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open file for writing: " + path.string());
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw std::runtime_error("failed to write file: " + path.string());
        }
    });
}

Task<void> Fs::append_file(std::filesystem::path path, std::string_view data) {
    co_await run_blocking([path = std::move(path), data] {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) {
            throw std::runtime_error("failed to open file for append: " + path.string());
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw std::runtime_error("failed to append file: " + path.string());
        }
    });
}

Task<void> Fs::copy_file(std::filesystem::path from, std::filesystem::path to, bool overwrite) {
    co_await run_blocking([from = std::move(from), to = std::move(to), overwrite] {
        std::error_code ec;
        const auto opts = overwrite ? std::filesystem::copy_options::overwrite_existing
                                    : std::filesystem::copy_options::none;
        const bool copied = std::filesystem::copy_file(from, to, opts, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        if (!copied && !overwrite) {
            throw std::runtime_error("destination exists: " + to.string());
        }
    });
}

Task<void> Fs::rename(std::filesystem::path from, std::filesystem::path to) {
    co_await run_blocking([from = std::move(from), to = std::move(to)] {
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if (ec) {
            throw std::system_error(ec);
        }
    });
}

Task<void> Fs::remove(std::filesystem::path path) {
    co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        (void)std::filesystem::remove(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
    });
}

Task<std::uint64_t> Fs::remove_all(std::filesystem::path path) {
    co_return co_await run_blocking([path = std::move(path)] -> std::uint64_t {
        std::error_code ec;
        const auto count = std::filesystem::remove_all(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return static_cast<std::uint64_t>(count);
    });
}

Task<std::string> Fs::current_path() {
    co_return co_await run_blocking([] {
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return cwd.string();
    });
}

Task<void> Fs::set_current_path(std::filesystem::path path) {
    co_await run_blocking([path = std::move(path)] {
        std::error_code ec;
        std::filesystem::current_path(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
    });
}

Task<std::string> Fs::temp_directory_path() {
    co_return co_await run_blocking([] {
        std::error_code ec;
        const auto value = std::filesystem::temp_directory_path(ec);
        if (ec) {
            throw std::system_error(ec);
        }
        return value.string();
    });
}

Task<std::string> Fs::create_temporary_directory(std::string pattern) {
    co_return co_await run_blocking([pattern = std::move(pattern)] {
        auto tpl = make_tmp_template_path(pattern, "flux_tmp_XXXXXX");
        std::vector<char> buffer(tpl.begin(), tpl.end());
        buffer.push_back('\0');
        char *created = ::mkdtemp(buffer.data());
        if (created == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        return std::string(created);
    });
}

Task<std::string> Fs::create_temporary_file(std::string pattern) {
    co_return co_await run_blocking([pattern = std::move(pattern)] {
        auto tpl = make_tmp_template_path(pattern, "flux_file_XXXXXX");
        std::vector<char> buffer(tpl.begin(), tpl.end());
        buffer.push_back('\0');
        const int fd = ::mkstemp(buffer.data());
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "mkstemp");
        }
        ::close(fd);
        return std::string(buffer.data());
    });
}

File::~File() {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

File::File(File &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

File &File::operator=(File &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (fd_ >= 0) {
        (void)::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
    return *this;
}

Task<File> File::open(std::filesystem::path path, OpenFlags flags, int mode) {
    const int native_flags = to_posix_open_flags(flags);
    const int fd = co_await run_blocking([path = std::move(path), native_flags, mode]() {
        const int value = ::open(path.c_str(), native_flags, mode);
        if (value < 0) {
            throw std::system_error(errno, std::generic_category(), "open");
        }
        return value;
    });
    co_return File(fd);
}

bool File::valid() const noexcept {
    return fd_ >= 0;
}

int File::native_handle() const noexcept {
    return fd_;
}

Task<std::size_t> File::write_some(std::string_view data) {
    if (!valid()) {
        throw std::runtime_error("flux::File is not open");
    }
    if (data.empty()) {
        co_return 0;
    }

    const int fd = fd_;
    const std::size_t written = co_await run_blocking([fd, data]() -> std::size_t {
        const ssize_t rc = ::write(fd, data.data(), data.size());
        if (rc < 0) {
            throw std::system_error(errno, std::generic_category(), "write");
        }
        return static_cast<std::size_t>(rc);
    });

    co_return written;
}

Task<std::size_t> File::write_all(std::string_view data) {
    if (!valid()) {
        throw std::runtime_error("flux::File is not open");
    }

    const int fd = fd_;
    const std::size_t written = co_await run_blocking([fd, data]() -> std::size_t {
        std::size_t total = 0;
        while (total < data.size()) {
            const char *ptr = data.data() + total;
            const std::size_t remain = data.size() - total;
            const ssize_t rc = ::write(fd, ptr, remain);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "write");
            }
            if (rc == 0) {
                break;
            }
            total += static_cast<std::size_t>(rc);
        }
        return total;
    });

    co_return written;
}

Task<void> File::close() {
    if (!valid()) {
        co_return;
    }

    const int fd = std::exchange(fd_, -1);
    co_await run_blocking([fd] {
        if (::close(fd) != 0) {
            throw std::system_error(errno, std::generic_category(), "close");
        }
    });
}

Task<std::string> read_file(std::filesystem::path path) {
    co_return co_await Fs::read_file(std::move(path));
}

Task<void> write_file(std::filesystem::path path, std::string_view data) {
    co_await Fs::write_file(std::move(path), data);
}

Task<void> append_file(std::filesystem::path path, std::string_view data) {
    co_await Fs::append_file(std::move(path), data);
}

} // namespace flux
