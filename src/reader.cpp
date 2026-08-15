#include "reader.h"

#include <fstream>

std::optional<Document> read_file(const std::filesystem::path& path, std::error_code& ec) {
    if (!std::filesystem::exists(path, ec)) {
        if (!ec) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ec = std::make_error_code(std::errc::io_error);
        return std::nullopt;
    }

    std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    if (!in.good() && !in.eof()) {
        ec = std::make_error_code(std::errc::io_error);
        return std::nullopt;
    }

    Document doc;
    doc.path = path;
    doc.size = content.size();
    doc.content = std::move(content);

    return doc;
}

