#pragma once

#include "index.h"

#include <filesystem>
#include <system_error>
#include <vector>
#include <cstdint>

// One row of the "documents" table in findx.db.
// Represents a single indexed file's identity and metadata — no file content stored here (deliberate v0.5 decision).
struct DocumentRecord {
    DocID id;                       // stable ID, persists across runs (v0.6 requirement)
    std::filesystem::path path;
    std::uintmax_t size;
    std::int64_t mtime;              // raw file_time_type ticks, NOT a human-readable timestamp (see mtime_signature in main.cpp)
    std::size_t token_count;         // used by BM25's length normalization at search time
};

// A lightweight version of DocumentRecord, used only for change-detection (v0.6).
// Deliberately excludes token_count — the diff step only needs path/size/mtime to decide new vs changed vs unchanged.
struct ManifestEntry {
    DocID id;
    std::filesystem::path path;
    std::uintmax_t size;
    std::int64_t mtime;
};

// NEW (v1.0): one row of the "chunks" table — a slice of a document's raw text, used for semantic search.
// No "id" field: SQLite auto-assigns chunk IDs on insert, since nothing needs to know a chunk's ID before it's written.
struct ChunkRecord {
    DocID doc_id;                    // which document this chunk came from
    std::size_t chunk_index;         // this chunk's position within its document (0, 1, 2...)
    std::string text;                // the chunk's actual text, UNMODIFIED — no lowercasing, no punctuation stripped
};


struct SemanticResult {
    DocID doc_id;
    std::filesystem::path path;
    std::string chunk_text;
    std::size_t chunk_index;
    double distance;
};

std::vector<SemanticResult> search_semantic(
    const std::filesystem::path& db_path,
    const std::vector<char>& query_embedding_bytes,
    int top_k,
    std::error_code& ec);

// Full rebuild: wipes and rewrites the entire database. Used nowhere anymore in main.cpp (v0.6 replaced this
// flow with sync_index), but kept available/tested since it's a simpler, known-correct baseline.
bool save_index(const std::filesystem::path& db_path,
                 const std::vector<DocumentRecord>& documents,
                 const InvertedIndex& index,
                 std::error_code& ec);

// Loads everything back into memory for `findx search` — full documents + full postings.
bool load_index(const std::filesystem::path& db_path,
                 std::vector<DocumentRecord>& documents,
                 InvertedIndex& index,
                 std::error_code& ec);

// Loads ONLY the lightweight manifest — used by `findx index` at the start of every run, to know what's
// already indexed before crawling and diffing against it.
bool load_manifest(const std::filesystem::path& db_path,
                    std::vector<ManifestEntry>& manifest,
                    std::error_code& ec);

// Incremental update (v0.6, extended in v1.0): applies exactly the changes needed —
// deletes removed documents/postings/chunks, replaces changed documents' postings/chunks,
// inserts new documents/postings/chunks. Never touches unchanged documents' rows at all.
bool sync_index(const std::filesystem::path& db_path,
                 const std::vector<DocumentRecord>& new_or_changed_documents,
                 const InvertedIndex& new_postings_index,
                 const std::vector<ChunkRecord>& new_chunks,   // NEW (v1.0) parameter
                 const std::vector<DocID>& deleted_ids,
                 std::error_code& ec);


bool test_load_vec_extension(const std::filesystem::path& db_path, std::error_code& ec);