#pragma once

#include <fcntl.h>
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#include <concepts>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <uv.h>

#include "async_uv/cancel.h"
#include "async_uv/runtime.h"
#include "async_uv/stream.h"

namespace async_uv {

#ifndef R_OK
#define R_OK 4
#endif

#ifndef W_OK
#define W_OK 2
#endif

#ifndef X_OK
#define X_OK 1
#endif

template <typename Enum>
struct enable_bitmask_operators : std::false_type {};

template <typename Enum>
concept bitmask_enum = std::is_enum_v<Enum> && enable_bitmask_operators<Enum>::value;

template <bitmask_enum Enum>
constexpr Enum operator|(Enum lhs, Enum rhs) noexcept {
    return static_cast<Enum>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

template <bitmask_enum Enum>
constexpr Enum operator&(Enum lhs, Enum rhs) noexcept {
    return static_cast<Enum>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

template <bitmask_enum Enum>
constexpr Enum &operator|=(Enum &lhs, Enum rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

template <bitmask_enum Enum>
constexpr Enum &operator&=(Enum &lhs, Enum rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

template <bitmask_enum Enum>
constexpr bool has_flag(Enum value, Enum flag) noexcept {
    return (std::to_underlying(value) & std::to_underlying(flag)) == std::to_underlying(flag);
}

namespace path {

inline std::filesystem::path to_path(std::string_view value) {
    return std::filesystem::path(std::string(value));
}

inline std::string join() {
    return {};
}

template <typename... Parts>
    requires((std::constructible_from<std::string_view, Parts> && ...))
inline std::string join(std::string_view first, Parts &&...rest) {
    std::filesystem::path result{std::string(first)};
    ((result /= std::filesystem::path(std::string(std::string_view(std::forward<Parts>(rest))))),
     ...);
    return result.string();
}

inline std::string normalize(std::string_view value) {
    return to_path(value).lexically_normal().string();
}

inline std::string absolute(std::string_view value) {
    return std::filesystem::absolute(to_path(value)).string();
}

inline std::string filename(std::string_view value) {
    return to_path(value).filename().string();
}

inline std::string stem(std::string_view value) {
    return to_path(value).stem().string();
}

inline std::string extension(std::string_view value) {
    return to_path(value).extension().string();
}

inline std::string parent(std::string_view value) {
    return to_path(value).parent_path().string();
}

inline bool is_absolute(std::string_view value) {
    return to_path(value).is_absolute();
}

inline bool is_relative(std::string_view value) {
    return to_path(value).is_relative();
}

inline std::string relative(std::string_view value, std::string_view base) {
    return to_path(value).lexically_relative(to_path(base)).string();
}

} // namespace path

enum class OpenFlags : int {
    none = 0,
    read_only = O_RDONLY,
    write_only = O_WRONLY,
    read_write = O_RDWR,
    append = O_APPEND,
    create = O_CREAT,
    truncate = O_TRUNC,
    exclusive = O_EXCL,
};

template <>
struct enable_bitmask_operators<OpenFlags> : std::true_type {};

enum class AccessFlags : int {
    none = 0,
    exists = 0,
    read = R_OK,
    write = W_OK,
    execute = X_OK,
};

template <>
struct enable_bitmask_operators<AccessFlags> : std::true_type {};

enum class CopyFlags : int {
    none = 0,
    exclusive = UV_FS_COPYFILE_EXCL,
    clone = UV_FS_COPYFILE_FICLONE,
    clone_force = UV_FS_COPYFILE_FICLONE_FORCE,
};

template <>
struct enable_bitmask_operators<CopyFlags> : std::true_type {};

enum class SymlinkFlags : int {
    none = 0,
    directory = UV_FS_SYMLINK_DIR,
    junction = UV_FS_SYMLINK_JUNCTION,
};

template <>
struct enable_bitmask_operators<SymlinkFlags> : std::true_type {};

struct FileInfo {
    std::uint64_t size = 0;
    int mode = 0;
    std::uint64_t inode = 0;
    std::uint64_t device = 0;
    int uid = 0;
    int gid = 0;
    uv_timespec_t access_time{};
    uv_timespec_t modify_time{};
    uv_timespec_t change_time{};
    uv_timespec_t birth_time{};

    bool is_file() const noexcept;
    bool is_directory() const noexcept;
    bool is_symlink() const noexcept;
};

struct DirectoryEntry {
    std::string name;
    uv_dirent_type_t type = UV_DIRENT_UNKNOWN;

    bool is_file() const noexcept;
    bool is_directory() const noexcept;
    bool is_symlink() const noexcept;
};

struct FilesystemInfo {
    std::uint64_t type = 0;
    std::uint64_t block_size = 0;
    std::uint64_t blocks = 0;
    std::uint64_t blocks_free = 0;
    std::uint64_t blocks_available = 0;
    std::uint64_t files = 0;
    std::uint64_t files_free = 0;
    std::uint64_t fragment_size = 0;
};

class Directory {
public:
    using next_type = std::optional<DirectoryEntry>;
    using task_type = Task<next_type>;
    using stream_type = Stream<DirectoryEntry>;

    Directory() = default;
    ~Directory();

    Directory(const Directory &) = delete;
    Directory &operator=(const Directory &) = delete;
    Directory(Directory &&other) noexcept;
    Directory &operator=(Directory &&other) noexcept;

    static Task<Directory> open(std::string path);

    bool is_open() const noexcept;

    task_type read();
    stream_type entries();
    Task<void> close();

private:
    Directory(Runtime *runtime, uv_dir_t *dir) noexcept;
    void reset() noexcept;

    Runtime *runtime_ = nullptr;
    uv_dir_t *dir_ = nullptr;
    bool eof_ = false;

    friend class Fs;
};

class File {
public:
    File() = default;
    ~File();

    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&other) noexcept;
    File &operator=(File &&other) noexcept;

    static Task<File> open(std::string path, int flags, int mode = 0644);
    static Task<File> open(std::string path, OpenFlags flags, int mode = 0644);

    bool is_open() const noexcept;

    Task<std::string> read_some(std::size_t max_bytes = 64 * 1024);
    Task<std::string> read_some_at(std::int64_t offset, std::size_t max_bytes = 64 * 1024);
    Task<std::string> read_all(std::size_t chunk_size = 64 * 1024);
    Task<std::size_t> write_some(std::string_view data);
    Task<std::size_t> write_some_at(std::int64_t offset, std::string_view data);
    Task<std::size_t> write_all(std::string_view data);
    Task<std::size_t> write_all_at(std::int64_t offset, std::string_view data);
    Task<FileInfo> stat();
    Task<void> truncate(std::uint64_t size);
    Task<void> sync();
    Task<void> datasync();
    Task<void> chmod(int mode);
    Task<void> chown(uv_uid_t uid, uv_gid_t gid);
    Task<void> utime(double access_time, double modify_time);
    Task<std::size_t> send_to(File &destination, std::uint64_t length, std::int64_t offset = 0);
    Task<void> close();
    template <typename Rep, typename Period>
    Task<std::string> read_some_for(std::chrono::duration<Rep, Period> timeout,
                                    std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_timeout(timeout, read_some(max_bytes));
    }
    template <typename Rep, typename Period>
    Task<std::string> read_some_at_for(std::int64_t offset,
                                       std::chrono::duration<Rep, Period> timeout,
                                       std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_timeout(timeout, read_some_at(offset, max_bytes));
    }
    template <typename Rep, typename Period>
    Task<std::string> read_all_for(std::chrono::duration<Rep, Period> timeout,
                                   std::size_t chunk_size = 64 * 1024) {
        co_return co_await async_uv::with_timeout(timeout, read_all(chunk_size));
    }
    template <typename Rep, typename Period>
    Task<std::size_t> write_some_for(std::string_view data,
                                     std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, write_some(data));
    }
    template <typename Rep, typename Period>
    Task<std::size_t> write_some_at_for(std::int64_t offset,
                                        std::string_view data,
                                        std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, write_some_at(offset, data));
    }
    template <typename Rep, typename Period>
    Task<std::size_t> write_all_for(std::string_view data,
                                    std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, write_all(data));
    }
    template <typename Rep, typename Period>
    Task<std::size_t> write_all_at_for(std::int64_t offset,
                                       std::string_view data,
                                       std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, write_all_at(offset, data));
    }
    template <typename Rep, typename Period>
    Task<FileInfo> stat_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, stat());
    }
    template <typename Rep, typename Period>
    Task<void> close_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, close());
    }
    template <typename Clock, typename Duration>
    Task<std::string> read_some_until(std::chrono::time_point<Clock, Duration> deadline,
                                      std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_deadline(deadline, read_some(max_bytes));
    }
    template <typename Clock, typename Duration>
    Task<std::string> read_some_at_until(std::int64_t offset,
                                         std::chrono::time_point<Clock, Duration> deadline,
                                         std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_deadline(deadline, read_some_at(offset, max_bytes));
    }
    template <typename Clock, typename Duration>
    Task<std::string> read_all_until(std::chrono::time_point<Clock, Duration> deadline,
                                     std::size_t chunk_size = 64 * 1024) {
        co_return co_await async_uv::with_deadline(deadline, read_all(chunk_size));
    }
    template <typename Clock, typename Duration>
    Task<std::size_t> write_some_until(std::string_view data,
                                       std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, write_some(data));
    }
    template <typename Clock, typename Duration>
    Task<std::size_t> write_some_at_until(std::int64_t offset,
                                          std::string_view data,
                                          std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, write_some_at(offset, data));
    }
    template <typename Clock, typename Duration>
    Task<std::size_t> write_all_until(std::string_view data,
                                      std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, write_all(data));
    }
    template <typename Clock, typename Duration>
    Task<std::size_t> write_all_at_until(std::int64_t offset,
                                         std::string_view data,
                                         std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, write_all_at(offset, data));
    }
    template <typename Clock, typename Duration>
    Task<FileInfo> stat_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, stat());
    }
    template <typename Clock, typename Duration>
    Task<void> close_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, close());
    }

private:
    File(Runtime *runtime, uv_file file) noexcept;
    void reset() noexcept;

    Runtime *runtime_ = nullptr;
    uv_file file_ = static_cast<uv_file>(-1);

    friend class Fs;
};

struct TempFile {
    File file;
    std::string path;
};

class Fs {
public:
    static Task<Directory> open_directory(std::string path);
    static Task<File> open(std::string path, int flags, int mode = 0644);
    static Task<File> open(std::string path, OpenFlags flags, int mode = 0644);
    static Task<std::string> read_file(std::string path);
    static Task<void> write_file(std::string path, std::string_view content, int mode = 0644);
    static Task<void> append_file(std::string path, std::string_view content, int mode = 0644);
    static Task<void> create_file(std::string path, int mode = 0644);
    static Task<void> touch(std::string path, int mode = 0644);

    static Task<bool> exists(std::string path);
    static Task<bool> access(std::string path, int mode);
    static Task<bool> access(std::string path, AccessFlags mode);
    static Task<FileInfo> stat(std::string path);
    static Task<FileInfo> lstat(std::string path);
    static Task<std::vector<DirectoryEntry>> list_directory(std::string path);

    static Task<void> create_directory(std::string path, int mode = 0755);
    static Task<void> create_directories(std::string path, int mode = 0755);
    static Task<void> remove(std::string path);
    static Task<void> remove_file(std::string path);
    static Task<void> remove_directory(std::string path);
    static Task<std::size_t> remove_all(std::string path);
    static Task<void> copy_file(std::string from, std::string to, int flags = 0);
    static Task<void> copy_file(std::string from, std::string to, CopyFlags flags);
    static Task<void> copy(std::string from, std::string to, int copy_file_flags = 0);
    static Task<void> copy(std::string from, std::string to, CopyFlags copy_file_flags);
    static Task<void> move(std::string from, std::string to);
    static Task<void> rename(std::string from, std::string to);
    static Task<void> link(std::string from, std::string to);
    static Task<void> symlink(std::string target, std::string path, int flags = 0);
    static Task<void> symlink(std::string target, std::string path, SymlinkFlags flags);
    static Task<std::string> read_link(std::string path);
    static Task<std::string> real_path(std::string path);
    static Task<std::string> temp_directory();
    static Task<std::string> current_directory();
    static Task<void> chmod(std::string path, int mode);
    static Task<void> chown(std::string path, uv_uid_t uid, uv_gid_t gid);
    static Task<void> lchown(std::string path, uv_uid_t uid, uv_gid_t gid);
    static Task<void> utime(std::string path, double access_time, double modify_time);
    static Task<void> lutime(std::string path, double access_time, double modify_time);
    static Task<FilesystemInfo> statfs(std::string path);
    static Task<std::string> create_temporary_directory(std::string path_template);
    static Task<TempFile> create_temporary_file(std::string path_template);
    template <typename Rep, typename Period>
    static Task<File> open_for(std::string path,
                               int flags,
                               std::chrono::duration<Rep, Period> timeout,
                               int mode = 0644) {
        co_return co_await async_uv::with_timeout(timeout, open(std::move(path), flags, mode));
    }
    template <typename Rep, typename Period>
    static Task<File> open_for(std::string path,
                               OpenFlags flags,
                               std::chrono::duration<Rep, Period> timeout,
                               int mode = 0644) {
        co_return co_await async_uv::with_timeout(timeout, open(std::move(path), flags, mode));
    }
    template <typename Rep, typename Period>
    static Task<std::string> read_file_for(std::string path,
                                           std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, read_file(std::move(path)));
    }
    template <typename Rep, typename Period>
    static Task<void> write_file_for(std::string path,
                                     std::string_view content,
                                     std::chrono::duration<Rep, Period> timeout,
                                     int mode = 0644) {
        co_return co_await async_uv::with_timeout(timeout,
                                                  write_file(std::move(path), content, mode));
    }
    template <typename Rep, typename Period>
    static Task<void> append_file_for(std::string path,
                                      std::string_view content,
                                      std::chrono::duration<Rep, Period> timeout,
                                      int mode = 0644) {
        co_return co_await async_uv::with_timeout(timeout,
                                                  append_file(std::move(path), content, mode));
    }
    template <typename Rep, typename Period>
    static Task<bool> exists_for(std::string path, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, exists(std::move(path)));
    }
    template <typename Rep, typename Period>
    static Task<FileInfo> stat_for(std::string path, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, stat(std::move(path)));
    }
    template <typename Rep, typename Period>
    static Task<std::vector<DirectoryEntry>>
    list_directory_for(std::string path, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, list_directory(std::move(path)));
    }
    template <typename Clock, typename Duration>
    static Task<File> open_until(std::string path,
                                 int flags,
                                 std::chrono::time_point<Clock, Duration> deadline,
                                 int mode = 0644) {
        co_return co_await async_uv::with_deadline(deadline, open(std::move(path), flags, mode));
    }
    template <typename Clock, typename Duration>
    static Task<File> open_until(std::string path,
                                 OpenFlags flags,
                                 std::chrono::time_point<Clock, Duration> deadline,
                                 int mode = 0644) {
        co_return co_await async_uv::with_deadline(deadline, open(std::move(path), flags, mode));
    }
    template <typename Clock, typename Duration>
    static Task<std::string> read_file_until(std::string path,
                                             std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, read_file(std::move(path)));
    }
    template <typename Clock, typename Duration>
    static Task<void> write_file_until(std::string path,
                                       std::string_view content,
                                       std::chrono::time_point<Clock, Duration> deadline,
                                       int mode = 0644) {
        co_return co_await async_uv::with_deadline(deadline,
                                                   write_file(std::move(path), content, mode));
    }
    template <typename Clock, typename Duration>
    static Task<void> append_file_until(std::string path,
                                        std::string_view content,
                                        std::chrono::time_point<Clock, Duration> deadline,
                                        int mode = 0644) {
        co_return co_await async_uv::with_deadline(deadline,
                                                   append_file(std::move(path), content, mode));
    }
    template <typename Clock, typename Duration>
    static Task<bool> exists_until(std::string path,
                                   std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, exists(std::move(path)));
    }
    template <typename Clock, typename Duration>
    static Task<FileInfo> stat_until(std::string path,
                                     std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, stat(std::move(path)));
    }
    template <typename Clock, typename Duration>
    static Task<std::vector<DirectoryEntry>>
    list_directory_until(std::string path, std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, list_directory(std::move(path)));
    }
};

Task<std::string> read_file(std::string path);
Task<void> write_file(std::string path, std::string_view content, int mode = 0644);

} // namespace async_uv
