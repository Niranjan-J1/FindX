#include "crawler.h"
#include "reader.h"
#include "index.h"
#include "tokenizer.h"
#include "ranker.h"
#include "storage.h"

#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <cstdint>
#include <filesystem>

namespace {

std::int64_t to_epoch_seconds(std::filesystem::file_time_type ftime) {
    using namespace std::chrono;

    auto sctp = time_point_cast<system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());

    return static_cast<std::int64_t>(system_clock::to_time_t(sctp));
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

int run_index(const std::filesystem::path& root, const std::filesystem::path& db_path) {
    std::error_code ec;
    std::vector<FileEntry> entries = crawl(root, ec);

    if (ec) {
        std::cerr << "Error crawling " << root << ": " << ec.message() << "\n";
        return 1;
    }

    std::vector<DocumentRecord> documents;
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

        DocumentRecord record;
        record.id = id;
        record.path = entry.path;
        record.size = entry.size;
        record.mtime = to_epoch_seconds(entry.last_write_time);
        record.token_count = index.document_length(id);

        documents.push_back(record);
    }

    std::error_code save_ec;
    if (!save_index(db_path, documents, index, save_ec)) {
        std::cerr << "Failed to save index: " << save_ec.message() << "\n";
        return 1;
    }

    std::cout << "Indexed " << documents.size() << " documents"
               << " | skipped (extension): " << skipped_extension
               << " | read failed: " << read_failed
               << " | saved to: " << db_path.string() << "\n";

    return 0;
}

int run_search(const std::string& query, const std::filesystem::path& db_path) {
    std::vector<DocumentRecord> documents;
    InvertedIndex index;

    std::error_code ec;
    if (!load_index(db_path, documents, index, ec)) {
        std::cerr << "Could not load index from " << db_path
                   << " (" << ec.message() << "). Run 'findx index <path>' first.\n";
        return 1;
    }

    std::vector<std::string> query_tokens = tokenize(query);
    if (query_tokens.empty()) {
        std::cout << "No valid search term.\n";
        return 0;
    }

    std::vector<ScoredDocument> ranked = rank_bm25(index, query_tokens);
    if (ranked.empty()) {
        std::cout << "No matches.\n";
        return 0;
    }

    std::cout << "Found " << ranked.size() << " document(s), ranked by relevance:\n";
    for (const auto& sd : ranked) {
        if (sd.id >= documents.size()) {
            continue;
        }
        std::cout << "  " << documents[sd.id].path.string()
                   << " (score: " << std::fixed << std::setprecision(3) << sd.score << ")\n";
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                   << "  findx index <path>\n"
                   << "  findx search <query>\n";
        return 1;
    }

    std::string command = argv[1];
    std::filesystem::path db_path = "findx.db";

    if (command == "index") {
        std::filesystem::path root = argv[2];
        return run_index(root, db_path);
    }

    if (command == "search") {
        std::string query;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) {
                query += " ";
            }
            query += argv[i];
        }
        return run_search(query, db_path);
    }

    std::cerr << "Unknown command: " << command << "\n"
               << "Usage:\n"
               << "  findx index <path>\n"
               << "  findx search <query>\n";
    return 1;
}