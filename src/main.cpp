#include "crawler.h"
#include "reader.h"
#include "index.h"
#include "tokenizer.h"
#include "ranker.h"
#include "storage.h"
#include "threadsafe_queue.h"
#include "chunker.h"

#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>

namespace {

std::int64_t mtime_signature(std::filesystem::file_time_type ftime) {
    return static_cast<std::int64_t>(ftime.time_since_epoch().count());
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

struct PendingFile {
    std::filesystem::path path;
    std::uintmax_t size;
    std::int64_t mtime;
    DocID id;
};

// new_chunks parameter added — worker now produces chunks alongside postings/document records
void run_worker(ThreadSafeQueue<PendingFile>& queue,
                 InvertedIndex& shared_index,
                 std::mutex& index_mutex,
                 std::vector<DocumentRecord>& new_or_changed,
                 std::vector<ChunkRecord>& new_chunks,
                 std::atomic<std::size_t>& read_failed)
{
    PendingFile item;
    while (queue.pop(item)) {
        std::error_code read_ec;
        auto doc = read_file(item.path, read_ec);

        if (!doc) {
            std::cerr << "Failed to read " << item.path << ": " << read_ec.message() << "\n";
            ++read_failed;
            continue;
        }

        std::vector<std::string> tokens = tokenize(doc->content);

        std::unordered_map<std::string, int> term_counts;
        for (const auto& term : tokens) {
            term_counts[term]++;
        }

        // NEW: chunk the same raw content for semantic search, separate from tokenize()'s output
        std::vector<std::string> text_chunks = chunk_text(doc->content);

        DocumentRecord record;
        record.id = item.id;
        record.path = item.path;
        record.size = item.size;
        record.mtime = item.mtime;
        record.token_count = tokens.size();

        {
            std::lock_guard<std::mutex> lock(index_mutex);
            shared_index.set_document_length(item.id, tokens.size());
            for (const auto& [term, count] : term_counts) {
                shared_index.set_posting(term, item.id, count);
            }
            new_or_changed.push_back(record);

            // NEW: push this document's chunks into the shared vector, same critical section as everything else
            for (std::size_t i = 0; i < text_chunks.size(); ++i) {
                ChunkRecord chunk;
                chunk.doc_id = item.id;
                chunk.chunk_index = i;
                chunk.text = text_chunks[i];
                new_chunks.push_back(std::move(chunk));
            }
        }
    }
}

int run_index(const std::filesystem::path& root, const std::filesystem::path& db_path) {
    std::vector<ManifestEntry> manifest;
    std::error_code manifest_ec;
    if (!load_manifest(db_path, manifest, manifest_ec)) {
        std::cerr << "Failed to load existing index manifest: " << manifest_ec.message() << "\n";
        return 1;
    }

    std::unordered_map<std::string, ManifestEntry> existing_by_path;
    DocID next_id = 0;
    for (const auto& entry : manifest) {
        existing_by_path[entry.path.string()] = entry;
        if (entry.id >= next_id) {
            next_id = entry.id + 1;
        }
    }

    std::error_code ec;
    std::vector<FileEntry> entries = crawl(root, ec);
    if (ec) {
        std::cerr << "Error crawling " << root << ": " << ec.message() << "\n";
        return 1;
    }

    ThreadSafeQueue<PendingFile> queue;
    std::unordered_set<std::string> seen_paths;

    std::size_t skipped_extension = 0;
    std::size_t unchanged_count = 0;
    std::size_t new_count = 0;
    std::size_t changed_count = 0;

    for (const auto& entry : entries) {
        if (!has_indexable_extension(entry.path)) {
            ++skipped_extension;
            continue;
        }

        std::string path_str = entry.path.string();
        seen_paths.insert(path_str);

        std::int64_t mtime = mtime_signature(entry.last_write_time);

        auto found = existing_by_path.find(path_str);
        DocID id;

        if (found == existing_by_path.end()) {
            id = next_id++;
            ++new_count;
            queue.push(PendingFile{ entry.path, entry.size, mtime, id });
        } else {
            id = found->second.id;
            if (found->second.size == entry.size && found->second.mtime == mtime) {
                ++unchanged_count;
            } else {
                ++changed_count;
                queue.push(PendingFile{ entry.path, entry.size, mtime, id });
            }
        }
    }
    queue.close();

    InvertedIndex delta_index;
    std::vector<DocumentRecord> new_or_changed;
    std::vector<ChunkRecord> new_chunks;  // NEW
    std::mutex index_mutex;
    std::atomic<std::size_t> read_failed{0};

    unsigned int thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) {
        thread_count = 4;
    }

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < thread_count; ++i) {
        workers.emplace_back(run_worker, std::ref(queue), std::ref(delta_index),
                              std::ref(index_mutex), std::ref(new_or_changed),
                              std::ref(new_chunks), std::ref(read_failed));
    }

    for (auto& t : workers) {
        t.join();
    }

    std::vector<DocID> deleted_ids;
    for (const auto& entry : manifest) {
        if (seen_paths.find(entry.path.string()) == seen_paths.end()) {
            deleted_ids.push_back(entry.id);
        }
    }

    std::error_code sync_ec;
    if (!sync_index(db_path, new_or_changed, delta_index, new_chunks, deleted_ids, sync_ec)) {
        std::cerr << "Failed to sync index: " << sync_ec.message() << "\n";
        return 1;
    }

    std::cout << "Index sync complete for: " << db_path.string() << "\n"
               << "  new: " << new_count
               << " | changed: " << changed_count
               << " | unchanged: " << unchanged_count
               << " | deleted: " << deleted_ids.size()
               << " | skipped (extension): " << skipped_extension
               << " | read failed: " << read_failed.load()
               << " | chunks written: " << new_chunks.size()  // NEW
               << " | threads used: " << thread_count << "\n";

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