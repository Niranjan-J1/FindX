#include "crawler.h"

std::vector<FileEntry> crawl(const std::filesystem::path& root, std::error_code& ec) {
    std::vector<FileEntry> results;

    if (!std::filesystem::exists(root, ec)) {
        if (!ec) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        return results;
    }

    if (!std::filesystem::is_directory(root, ec)) {
        if (!ec) {
            ec = std::make_error_code(std::errc::not_a_directory);
        }
        return results;
    }

    auto options = std::filesystem::directory_options::skip_permission_denied;

    for (auto it = std::filesystem::recursive_directory_iterator(root, options, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec) {
            break;
        }

        const auto& entry = *it;

        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) {
            continue;
        }

        std::error_code size_ec, time_ec;
        auto size = entry.file_size(size_ec);
        auto mtime = entry.last_write_time(time_ec);

        if (size_ec || time_ec) {
            continue;
        }

        results.push_back(FileEntry{ entry.path(), size, mtime });
    }

    return results;
}