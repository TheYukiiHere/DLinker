#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace flink {

constexpr std::size_t kMaxFileSize = 256 * 1024 * 1024;

struct FileEntry {
    std::string path;
    std::uint64_t size;
    std::int64_t modified;
};

inline std::int64_t modifiedTime(const std::filesystem::path& path)
{
    const auto value = std::filesystem::last_write_time(path).time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

inline std::vector<FileEntry> manifest(const std::filesystem::path& root)
{
    std::vector<FileEntry> result;
    if (!std::filesystem::exists(root)) {
        return result;
    }

    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        const auto filename = item.path().filename().string();
        if (!item.is_regular_file() || filename == ".flink-state" ||
            filename == "sync.log" || filename == "agent.log" ||
            filename == "flink.conf.txt" || item.path().extension() == ".zip") {
            continue;
        }
        result.push_back({
            item.path().lexically_relative(root).generic_string(),
            item.file_size(),
            modifiedTime(item.path())
        });
    }
    return result;
}

inline bool safeRelativePath(const std::string& value)
{
    const std::filesystem::path path(value);
    return !value.empty() && path.is_relative() && path.lexically_normal() == path &&
           value.find('\0') == std::string::npos;
}

inline bool writeFile(const std::filesystem::path& root, const std::string& relative,
                      const std::vector<std::uint8_t>& content)
{
    if (!safeRelativePath(relative)) {
        return false;
    }
    const auto target = root / std::filesystem::path(relative);
    std::filesystem::create_directories(target.parent_path());
    std::ofstream output(target, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    return output.good();
}

inline bool readFile(const std::filesystem::path& root, const std::string& relative,
                     std::vector<std::uint8_t>& content)
{
    if (!safeRelativePath(relative)) {
        return false;
    }
    const auto source = root / std::filesystem::path(relative);
    std::ifstream input(source, std::ios::binary | std::ios::ate);
    if (!input) {
        return false;
    }
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxFileSize) {
        return false;
    }
    content.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(content.data()), size);
    return input.good() || input.eof();
}

} // namespace flink
