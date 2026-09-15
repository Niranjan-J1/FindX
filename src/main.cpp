#include "crawler.h"
#include "reader.h"
#include "index.h"
#include "tokenizer.h"
#include "ranker.h"
#include "storage.h"
#include "threadsafe_queue.h"
#include "chunker.h"
#include "embed_client.h"
#include "ollama_client.h"

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
#include <sstream>

namespace {

const std::string MODEL_FAST = "qwen3:1.7b";
const std::string MODEL_STRONG = "phi4-mini";

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
    std::vector<ChunkRecord> new_chunks;
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
               << " | chunks written: " << new_chunks.size()
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
    std::vector<ScoredDocument> bm25_results;
    if (!query_tokens.empty()) {
        bm25_results = rank_bm25(index, query_tokens);
    }

    std::vector<SemanticResult> semantic_results;
    bool semantic_available = false;

    std::error_code embed_ec;
    auto embedding = get_query_embedding(query, embed_ec);
    if (!embed_ec && !embedding.empty()) {
        auto query_bytes = serialize_float_vector(embedding);
        std::error_code search_ec;
        semantic_results = search_semantic(db_path, query_bytes, 10, search_ec);
        if (!search_ec) {
            semantic_available = true;
        }
    }

    if (bm25_results.empty() && semantic_results.empty()) {
        std::cout << "No matches.\n";
        return 0;
    }

    auto hybrid = merge_hybrid(bm25_results, documents, semantic_results, 10);

    if (!semantic_available) {
        std::cout << "(semantic search unavailable — showing keyword results only)\n\n";
    }

    std::cout << "Found " << hybrid.size() << " result(s):\n\n";
    for (const auto& r : hybrid) {
        std::cout << "  " << r.path.string()
                   << " (relevance: " << std::fixed << std::setprecision(4) << r.rrf_score << ")\n";
        if (!r.chunk_preview.empty()) {
            std::cout << "    " << r.chunk_preview << "\n";
        }
        std::cout << "\n";
    }

    return 0;
}

struct RoutingDecision {
    std::string model;
    std::string reason;
};

RoutingDecision auto_pick_model(
    const std::vector<HybridResult>& hybrid,
    const std::vector<SemanticResult>& semantic_results,
    const std::vector<ScoredDocument>& bm25_results)
{
    // Signal 1: how confident is the best semantic match?
    // low distance = strong match = simpler answer likely sufficient
    double best_distance = 999.0;
    if (!semantic_results.empty()) {
        best_distance = semantic_results[0].distance;
    }

    // Signal 2: how concentrated are sources?
    // all results from one file = focused question, few files = simpler
    std::unordered_set<std::string> unique_source_files;
    for (const auto& r : hybrid) {
        unique_source_files.insert(r.path.string());
    }

    // Signal 3: do BM25 and semantic agree on the top result?
    bool systems_agree = false;
    if (!bm25_results.empty() && !semantic_results.empty() && !hybrid.empty()) {
        // check if the same doc appears in top 2 of both systems
        std::unordered_set<DocID> bm25_top;
        for (std::size_t i = 0; i < std::min(bm25_results.size(), static_cast<std::size_t>(2)); ++i) {
            bm25_top.insert(bm25_results[i].id);
        }
        for (std::size_t i = 0; i < std::min(semantic_results.size(), static_cast<std::size_t>(2)); ++i) {
            if (bm25_top.count(semantic_results[i].doc_id)) {
                systems_agree = true;
                break;
            }
        }
    }

    // Decision logic:
    // strong single-source match with system agreement → fast model handles this easily
    // weak/scattered matches or system disagreement → harder synthesis, use strong model
    bool use_fast = false;
    std::string reason;

    if (best_distance < 1.1 && unique_source_files.size() <= 2 && systems_agree) {
        use_fast = true;
        reason = "strong focused match, both systems agree";
    } else if (best_distance < 1.05 && unique_source_files.size() <= 2) {
        use_fast = true;
        reason = "very strong single-source match";
    } else if (unique_source_files.size() >= 4 || best_distance > 1.3) {
        use_fast = false;
        reason = "scattered sources or weak matches, needs deeper reasoning";
    } else {
        use_fast = false;
        reason = "moderate complexity, using stronger model for quality";
    }

    return RoutingDecision{
        use_fast ? MODEL_FAST : MODEL_STRONG,
        reason
    };
}

int run_ask(const std::string& question, const std::string& mode, const std::filesystem::path& db_path) {
    // Step 1: hybrid retrieval
    std::vector<DocumentRecord> documents;
    InvertedIndex index;

    std::error_code ec;
    if (!load_index(db_path, documents, index, ec)) {
        std::cerr << "Could not load index. Run 'findx index <path>' first.\n";
        return 1;
    }

    std::vector<std::string> query_tokens = tokenize(question);
    std::vector<ScoredDocument> bm25_results;
    if (!query_tokens.empty()) {
        bm25_results = rank_bm25(index, query_tokens);
    }

    std::vector<SemanticResult> semantic_results;
    std::error_code embed_ec;
    auto embedding = get_query_embedding(question, embed_ec);
    if (!embed_ec && !embedding.empty()) {
        auto query_bytes = serialize_float_vector(embedding);
        std::error_code search_ec;
        semantic_results = search_semantic(db_path, query_bytes, 10, search_ec);
    }

    auto hybrid = merge_hybrid(bm25_results, documents, semantic_results, 5);

    if (hybrid.empty()) {
        std::cout << "No relevant sources found for that question.\n";
        return 0;
    }

    // Step 2: build prompt with numbered sources
    std::ostringstream prompt;
    prompt << "You are a helpful assistant that answers questions based ONLY on the provided sources. "
           << "Cite sources using [1], [2], etc. after each claim. "
           << "If the sources don't contain enough information, say so.\n\n";

    struct Source {
        std::filesystem::path path;
        std::string preview;
    };
    std::vector<Source> sources;
    std::unordered_map<std::string, std::size_t> seen_paths;

    for (const auto& r : hybrid) {
        std::string path_str = r.path.string();
        if (seen_paths.find(path_str) == seen_paths.end()) {
            std::size_t num = sources.size() + 1;
            seen_paths[path_str] = num;
            sources.push_back(Source{ r.path, r.chunk_preview });
        }
    }

    for (const auto& r : hybrid) {
        std::string path_str = r.path.string();
        std::size_t num = seen_paths[path_str];

        std::string content = r.chunk_preview;
        for (const auto& sr : semantic_results) {
            if (sr.path.string() == path_str && sr.chunk_text.size() > content.size()) {
                content = sr.chunk_text;
            }
        }

        prompt << "[Source " << num << ": " << r.path.filename().string() << "]\n"
               << content << "\n\n";
    }

    prompt << "Question: " << question << "\n"
           << "Answer:";

    // Step 3: pick model
    std::string model;
    if (mode == "fast") {
        model = MODEL_FAST;
        std::cout << "[--fast: using " << model << "]\n\n";
    } else if (mode == "deep") {
        model = MODEL_STRONG;
        std::cout << "[--deep: using " << model << "]\n\n";
    } else {
        auto decision = auto_pick_model(hybrid, semantic_results, bm25_results);
        model = decision.model;
        std::cout << "[auto: using " << model << " — " << decision.reason << "]\n\n";
    }

    // Step 4: stream the response
    std::string full_answer;
    std::error_code ollama_ec;

    query_ollama_stream(prompt.str(), model,
        [&full_answer](const std::string& token) {
            std::cout << token << std::flush;
            full_answer += token;
        },
        ollama_ec);

    if (ollama_ec) {
        std::cerr << "\nCould not reach Ollama (is it running?): " << ollama_ec.message() << "\n";
        return 1;
    }

    // Step 5: print sources
    std::cout << "\n\n--- Sources ---\n";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        std::cout << "[" << (i + 1) << "] " << sources[i].path.string() << "\n";
        if (!sources[i].preview.empty()) {
            std::cout << "    " << sources[i].preview << "\n";
        }
    }
    std::cout << "\n";

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                   << "  findx index <path>\n"
                   << "  findx search <query>\n"
                   << "  findx ask [--fast|--deep] <question>\n";
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
            if (i > 2) query += " ";
            query += argv[i];
        }
        return run_search(query, db_path);
    }

    if (command == "ask") {
        std::string mode = "auto";
        int query_start = 2;

        if (argc > 2 && std::string(argv[2]) == "--fast") {
            mode = "fast";
            query_start = 3;
        } else if (argc > 2 && std::string(argv[2]) == "--deep") {
            mode = "deep";
            query_start = 3;
        }

        if (query_start >= argc) {
            std::cerr << "Missing question after " << argv[2] << "\n";
            return 1;
        }

        std::string question;
        for (int i = query_start; i < argc; ++i) {
            if (i > query_start) question += " ";
            question += argv[i];
        }
        return run_ask(question, mode, db_path);
    }

    std::cerr << "Unknown command: " << command << "\n"
               << "Usage:\n"
               << "  findx index <path>\n"
               << "  findx search <query>\n"
               << "  findx ask [--fast|--deep] <question>\n";
    return 1;
}