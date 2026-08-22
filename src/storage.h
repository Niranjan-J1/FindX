#pragma once

#include "index.h"

#include <filesystem>
#include <system_error>
#include <vector>
#include <cstdint>

struct DocumentRecord {
    DocID id;
    std::filesystem::path path;
    std::uintmax_t size;
    std::int64_t mtime;
    std::size_t token_count;
};

bool save_index(const std::filesystem::path& db_path,
                 const std::vector<DocumentRecord>& documents,
                 const InvertedIndex& index,
                 std::error_code& ec);

bool load_index(const std::filesystem::path& db_path,
                 std::vector<DocumentRecord>& documents,
                 InvertedIndex& index,
                 std::error_code& ec);