#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "flux/cancel.h"
#include "flux/runtime.h"

namespace flux {

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

struct FileInfo {
    std::uint64_t size = 0;
    bool regular_file = false;
    bool directory = false;
    bool symlink = false;
    std::filesystem::file_time_type last_write_time{};

    bool is_file() const noexcept {
        return regular_file;
    }
    bool is_directory() const noexcept {
        return directory;
    }
    bool is_symlink() const noexcept {
        return symlink;
    }
};

struct DirectoryEntry {
    std::string name;
    std::string path;
    bool regular_file = false;
    bool directory = false;
    bool symlink = false;

    bool is_file() const noexcept {
        return regular_file;
    }
    bool is_directory() const noexcept {
        return directory;
    }
    bool is_symlink() const noexcept {
        return symlink;
    }
};

class Fs {
public:
    static Task<bool> exists(std::filesystem::path path);
    static Task<bool> is_file(std::filesystem::path path);
    static Task<bool> is_directory(std::filesystem::path path);
    static Task<FileInfo> stat(std::filesystem::path path);
    static Task<std::uint64_t> file_size(std::filesystem::path path);
    static Task<std::vector<DirectoryEntry>> list_directory(std::filesystem::path path);

    static Task<void> create_directories(std::filesystem::path path);
    static Task<void> create_directory(std::filesystem::path path);
    static Task<void> create_file(std::filesystem::path path);

    static Task<std::string> read_file(std::filesystem::path path);
    static Task<void> write_file(std::filesystem::path path, std::string_view data);
    static Task<void> append_file(std::filesystem::path path, std::string_view data);

    static Task<void>
    copy_file(std::filesystem::path from, std::filesystem::path to, bool overwrite = false);
    static Task<void> rename(std::filesystem::path from, std::filesystem::path to);
    static Task<void> remove(std::filesystem::path path);
    static Task<std::uint64_t> remove_all(std::filesystem::path path);

    static Task<std::string> current_path();
    static Task<void> set_current_path(std::filesystem::path path);
    static Task<std::string> temp_directory_path();
    static Task<std::string> create_temporary_directory(std::string pattern);
    static Task<std::string> create_temporary_file(std::string pattern);
};

Task<std::string> read_file(std::filesystem::path path);
Task<void> write_file(std::filesystem::path path, std::string_view data);
Task<void> append_file(std::filesystem::path path, std::string_view data);

} // namespace flux
