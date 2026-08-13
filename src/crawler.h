#pragma once

#include <filesystem>
#include <vector>
#include <system_error>

struct FileEntry {
    std::filesystem::path path;
    std::uintmax_t size;
    std::filesystem::file_time_type last_write_time;
};

std::vector<FileEntry> crawl(const std::filesystem::path& root, std::error_code& ec);