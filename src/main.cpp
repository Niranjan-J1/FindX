#include "crawler.h"
#include "reader.h"
#include "index.h"
#include "tokenizer.h"

#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {

std::string format_time(std::filesystem::file_time_type ftime) {
    using namespace std::chrono;

    auto sctp = time_point_cast<system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());

    std::time_t tt = system_clock::to_time_t(sctp);
    std::tm* tm = std::localtime(&tt);

    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

bool has_indexable_extension(const std::filesystem::path& path) {
    static const std::array<std::string, 4> allowed = { ".txt", ".md", ".cpp", ".h" };

    std::string ext = path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (const auto& a : allowed) {
        if (ext == a) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[1]) != "index") {
        std::cerr << "Usage: findx index <path>\n";
        return 1;
    }

    std::filesystem::path root = argv[2];

    std::error_code ec;
    std::vector<FileEntry> entries = crawl(root, ec);

    if (ec) {
        std::cerr << "Error crawling " << root << ": " << ec.message() << "\n";
        return 1;
    }

    std::vector<Document> documents;
    InvertedIndex index;

    std::size_t skipped_extension = 0;
    std::size_t read_failed = 0;

    for (const auto& entry : entries) {
        if (!has_indexable_extension(entry.path)) {
            ++skipped_extension;
            continue;
        }

        std::error_code read_ec;
        auto doc = read_file(entry.path, read_ec);

        if (!doc) {
            std::cerr << "Failed to read " << entry.path << ": " << read_ec.message() << "\n";
            ++read_failed;
            continue;
        }

        DocID id = documents.size();
        index.add_document(id, doc->content);
        documents.push_back(std::move(*doc));
    }

    std::cout << "Indexed " << documents.size() << " documents"
               << " | skipped (extension): " << skipped_extension
               << " | read failed: " << read_failed << "\n";

    std::cout << "\nEnter a term to search (blank line to quit):\n";

    std::string query;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, query) || query.empty()) {
            break;
        }

        std::vector<std::string> query_tokens = tokenize(query);
        if (query_tokens.empty()) {
            std::cout << "No valid search term.\n";
            continue;
        }

        const std::string& term = query_tokens.front();
        const auto* results = index.lookup(term);
        if (!results) {
            std::cout << "No matches.\n";
            continue;
        }

        std::cout << "Found in " << results->size() << " document(s):\n";
        for (const auto& [id, count] : *results) {
            std::cout << "  " << documents[id].path.string()
                       << " (" << count << " occurrence" << (count == 1 ? "" : "s") << ")\n";
        }
    }

    return 0;
}