#pragma once 

#include <filesystem>
#include <string>
#include <system_error>
#include <optional>
#include <cstdint>

struct Document{

    std::filesystem::path path;
    std::string content;
    std::uintmax_t size; 

};


std::optional<Document> read_file(const std::filesystem::path& path, std::error_code& ec);