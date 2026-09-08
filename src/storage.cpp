#include "storage.h"

#include <sqlite3.h>
#include <iostream>

namespace {

bool exec_sql(sqlite3* db, const char* sql, std::error_code& ec) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite error: " << (err_msg ? err_msg : "unknown") << "\n";
        sqlite3_free(err_msg);
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    return true;
}

// NEW: chunks table added alongside documents/postings
const char* CREATE_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS documents ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  size INTEGER NOT NULL,"
    "  mtime INTEGER NOT NULL,"
    "  token_count INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS postings ("
    "  term TEXT NOT NULL,"
    "  doc_id INTEGER NOT NULL REFERENCES documents(id),"
    "  count INTEGER NOT NULL,"
    "  PRIMARY KEY (term, doc_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS chunks ("
    "  id INTEGER PRIMARY KEY,"
    "  doc_id INTEGER NOT NULL REFERENCES documents(id),"
    "  chunk_index INTEGER NOT NULL,"
    "  text TEXT NOT NULL"
    ");";

} // namespace

bool save_index(const std::filesystem::path& db_path,
                 const std::vector<DocumentRecord>& documents,
                 const InvertedIndex& index,
                 std::error_code& ec)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    const char* drop_sql =
        "DROP TABLE IF EXISTS postings;"
        "DROP TABLE IF EXISTS chunks;"
        "DROP TABLE IF EXISTS documents;";
    if (!exec_sql(db, drop_sql, ec)) { sqlite3_close(db); return false; }
    if (!exec_sql(db, CREATE_SCHEMA_SQL, ec)) { sqlite3_close(db); return false; }
    if (!exec_sql(db, "BEGIN TRANSACTION;", ec)) { sqlite3_close(db); return false; }

    sqlite3_stmt* doc_stmt = nullptr;
    const char* doc_sql =
        "INSERT INTO documents (id, path, size, mtime, token_count) VALUES (?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, doc_sql, -1, &doc_stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    for (const auto& doc : documents) {
        std::string path_str = doc.path.string();

        sqlite3_bind_int64(doc_stmt, 1, static_cast<sqlite3_int64>(doc.id));
        sqlite3_bind_text(doc_stmt, 2, path_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(doc_stmt, 3, static_cast<sqlite3_int64>(doc.size));
        sqlite3_bind_int64(doc_stmt, 4, doc.mtime);
        sqlite3_bind_int64(doc_stmt, 5, static_cast<sqlite3_int64>(doc.token_count));

        if (sqlite3_step(doc_stmt) != SQLITE_DONE) {
            ec = std::make_error_code(std::errc::io_error);
            sqlite3_finalize(doc_stmt);
            sqlite3_close(db);
            return false;
        }
        sqlite3_reset(doc_stmt);
    }
    sqlite3_finalize(doc_stmt);

    sqlite3_stmt* posting_stmt = nullptr;
    const char* posting_sql = "INSERT INTO postings (term, doc_id, count) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db, posting_sql, -1, &posting_stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    for (const auto& [term, doc_map] : index.all_postings()) {
        for (const auto& [id, count] : doc_map) {
            sqlite3_bind_text(posting_stmt, 1, term.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(posting_stmt, 2, static_cast<sqlite3_int64>(id));
            sqlite3_bind_int(posting_stmt, 3, count);

            if (sqlite3_step(posting_stmt) != SQLITE_DONE) {
                ec = std::make_error_code(std::errc::io_error);
                sqlite3_finalize(posting_stmt);
                sqlite3_close(db);
                return false;
            }
            sqlite3_reset(posting_stmt);
        }
    }
    sqlite3_finalize(posting_stmt);

    if (!exec_sql(db, "COMMIT;", ec)) { sqlite3_close(db); return false; }

    sqlite3_close(db);
    return true;
}

bool load_index(const std::filesystem::path& db_path,
                 std::vector<DocumentRecord>& documents,
                 InvertedIndex& index,
                 std::error_code& ec)
{
    if (!std::filesystem::exists(db_path, ec)) {
        if (!ec) { ec = std::make_error_code(std::errc::no_such_file_or_directory); }
        return false;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* doc_stmt = nullptr;
    const char* doc_sql = "SELECT id, path, size, mtime, token_count FROM documents;";
    if (sqlite3_prepare_v2(db, doc_sql, -1, &doc_stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    while (sqlite3_step(doc_stmt) == SQLITE_ROW) {
        DocID id = static_cast<DocID>(sqlite3_column_int64(doc_stmt, 0));
        const unsigned char* path_text = sqlite3_column_text(doc_stmt, 1);

        DocumentRecord record;
        record.id = id;
        record.path = std::filesystem::path(reinterpret_cast<const char*>(path_text));
        record.size = static_cast<std::uintmax_t>(sqlite3_column_int64(doc_stmt, 2));
        record.mtime = sqlite3_column_int64(doc_stmt, 3);
        record.token_count = static_cast<std::size_t>(sqlite3_column_int64(doc_stmt, 4));

        if (documents.size() <= id) {
            documents.resize(id + 1);
        }
        documents[id] = record;

        index.set_document_length(id, record.token_count);
    }
    sqlite3_finalize(doc_stmt);

    sqlite3_stmt* posting_stmt = nullptr;
    const char* posting_sql = "SELECT term, doc_id, count FROM postings;";
    if (sqlite3_prepare_v2(db, posting_sql, -1, &posting_stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    while (sqlite3_step(posting_stmt) == SQLITE_ROW) {
        const unsigned char* term_text = sqlite3_column_text(posting_stmt, 0);
        DocID id = static_cast<DocID>(sqlite3_column_int64(posting_stmt, 1));
        int count = sqlite3_column_int(posting_stmt, 2);

        index.set_posting(reinterpret_cast<const char*>(term_text), id, count);
    }
    sqlite3_finalize(posting_stmt);

    sqlite3_close(db);
    return true;
}

bool load_manifest(const std::filesystem::path& db_path,
                    std::vector<ManifestEntry>& manifest,
                    std::error_code& ec)
{
    if (!std::filesystem::exists(db_path, ec)) {
        ec.clear();
        return true;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, path, size, mtime FROM documents;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ManifestEntry entry;
        entry.id = static_cast<DocID>(sqlite3_column_int64(stmt, 0));
        const unsigned char* path_text = sqlite3_column_text(stmt, 1);
        entry.path = std::filesystem::path(reinterpret_cast<const char*>(path_text));
        entry.size = static_cast<std::uintmax_t>(sqlite3_column_int64(stmt, 2));
        entry.mtime = sqlite3_column_int64(stmt, 3);
        manifest.push_back(entry);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

bool sync_index(const std::filesystem::path& db_path,
                 const std::vector<DocumentRecord>& new_or_changed_documents,
                 const InvertedIndex& new_postings_index,
                 const std::vector<ChunkRecord>& new_chunks,
                 const std::vector<DocID>& deleted_ids,
                 std::error_code& ec)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    if (!exec_sql(db, CREATE_SCHEMA_SQL, ec)) { sqlite3_close(db); return false; }
    if (!exec_sql(db, "BEGIN TRANSACTION;", ec)) { sqlite3_close(db); return false; }

    // Deleted documents: clean up postings + chunks + the document row itself
    sqlite3_stmt* del_postings_stmt = nullptr;
    sqlite3_stmt* del_chunks_stmt = nullptr;
    sqlite3_stmt* del_doc_stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM postings WHERE doc_id = ?;", -1, &del_postings_stmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM chunks WHERE doc_id = ?;", -1, &del_chunks_stmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM documents WHERE id = ?;", -1, &del_doc_stmt, nullptr) != SQLITE_OK)
    {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    for (DocID id : deleted_ids) {
        sqlite3_bind_int64(del_postings_stmt, 1, static_cast<sqlite3_int64>(id));
        sqlite3_step(del_postings_stmt);
        sqlite3_reset(del_postings_stmt);

        sqlite3_bind_int64(del_chunks_stmt, 1, static_cast<sqlite3_int64>(id));
        sqlite3_step(del_chunks_stmt);
        sqlite3_reset(del_chunks_stmt);

        sqlite3_bind_int64(del_doc_stmt, 1, static_cast<sqlite3_int64>(id));
        sqlite3_step(del_doc_stmt);
        sqlite3_reset(del_doc_stmt);
    }
    sqlite3_finalize(del_postings_stmt);
    sqlite3_finalize(del_chunks_stmt);
    sqlite3_finalize(del_doc_stmt);

    // New/changed documents: clear old postings + chunks before upserting the document row
    sqlite3_stmt* clear_postings_stmt = nullptr;
    sqlite3_stmt* clear_chunks_stmt = nullptr;
    sqlite3_stmt* upsert_doc_stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM postings WHERE doc_id = ?;", -1, &clear_postings_stmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM chunks WHERE doc_id = ?;", -1, &clear_chunks_stmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO documents (id, path, size, mtime, token_count) VALUES (?, ?, ?, ?, ?);",
            -1, &upsert_doc_stmt, nullptr) != SQLITE_OK)
    {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    for (const auto& doc : new_or_changed_documents) {
        sqlite3_bind_int64(clear_postings_stmt, 1, static_cast<sqlite3_int64>(doc.id));
        sqlite3_step(clear_postings_stmt);
        sqlite3_reset(clear_postings_stmt);

        sqlite3_bind_int64(clear_chunks_stmt, 1, static_cast<sqlite3_int64>(doc.id));
        sqlite3_step(clear_chunks_stmt);
        sqlite3_reset(clear_chunks_stmt);

        std::string path_str = doc.path.string();
        sqlite3_bind_int64(upsert_doc_stmt, 1, static_cast<sqlite3_int64>(doc.id));
        sqlite3_bind_text(upsert_doc_stmt, 2, path_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upsert_doc_stmt, 3, static_cast<sqlite3_int64>(doc.size));
        sqlite3_bind_int64(upsert_doc_stmt, 4, doc.mtime);
        sqlite3_bind_int64(upsert_doc_stmt, 5, static_cast<sqlite3_int64>(doc.token_count));

        if (sqlite3_step(upsert_doc_stmt) != SQLITE_DONE) {
            ec = std::make_error_code(std::errc::io_error);
            sqlite3_finalize(clear_postings_stmt);
            sqlite3_finalize(clear_chunks_stmt);
            sqlite3_finalize(upsert_doc_stmt);
            sqlite3_close(db);
            return false;
        }
        sqlite3_reset(upsert_doc_stmt);
    }
    sqlite3_finalize(clear_postings_stmt);
    sqlite3_finalize(clear_chunks_stmt);
    sqlite3_finalize(upsert_doc_stmt);

    sqlite3_stmt* insert_posting_stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO postings (term, doc_id, count) VALUES (?, ?, ?);", -1, &insert_posting_stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    for (const auto& [term, doc_map] : new_postings_index.all_postings()) {
        for (const auto& [id, count] : doc_map) {
            sqlite3_bind_text(insert_posting_stmt, 1, term.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_posting_stmt, 2, static_cast<sqlite3_int64>(id));
            sqlite3_bind_int(insert_posting_stmt, 3, count);

            if (sqlite3_step(insert_posting_stmt) != SQLITE_DONE) {
                ec = std::make_error_code(std::errc::io_error);
                sqlite3_finalize(insert_posting_stmt);
                sqlite3_close(db);
                return false;
            }
            sqlite3_reset(insert_posting_stmt);
        }
    }
    sqlite3_finalize(insert_posting_stmt);

    // NEW: insert this run's chunks
    sqlite3_stmt* insert_chunk_stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO chunks (doc_id, chunk_index, text) VALUES (?, ?, ?);", -1, &insert_chunk_stmt, nullptr) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    for (const auto& chunk : new_chunks) {
        sqlite3_bind_int64(insert_chunk_stmt, 1, static_cast<sqlite3_int64>(chunk.doc_id));
        sqlite3_bind_int64(insert_chunk_stmt, 2, static_cast<sqlite3_int64>(chunk.chunk_index));
        sqlite3_bind_text(insert_chunk_stmt, 3, chunk.text.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insert_chunk_stmt) != SQLITE_DONE) {
            ec = std::make_error_code(std::errc::io_error);
            sqlite3_finalize(insert_chunk_stmt);
            sqlite3_close(db);
            return false;
        }
        sqlite3_reset(insert_chunk_stmt);
    }
    sqlite3_finalize(insert_chunk_stmt);

    if (!exec_sql(db, "COMMIT;", ec)) { sqlite3_close(db); return false; }

    sqlite3_close(db);
    return true;
}


bool test_load_vec_extension(const std::filesystem::path& db_path, std::error_code& ec) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    if (sqlite3_enable_load_extension(db, 1) != SQLITE_OK) {
        std::cerr << "Could not enable extension loading: " << sqlite3_errmsg(db) << "\n";
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    // path to the same vec0 extension Python has been using
    const char* vec_path = "C:\\Users\\niran\\Desktop\\FindX\\venv\\Lib\\site-packages\\sqlite_vec\\vec0";

    char* err_msg = nullptr;
    if (sqlite3_load_extension(db, vec_path, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Could not load vec extension: " << (err_msg ? err_msg : "unknown") << "\n";
        sqlite3_free(err_msg);
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    // simplest possible check that the extension actually works — count rows in chunk_vectors
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM chunk_vectors;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Query prepare failed: " << sqlite3_errmsg(db) << "\n";
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return false;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::cout << "chunk_vectors row count: " << sqlite3_column_int(stmt, 0) << "\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

std::vector<SemanticResult> search_semantic(
    const std::filesystem::path& db_path,
    const std::vector<char>& query_embedding_bytes,
    int top_k,
    std::error_code& ec)
{
    std::vector<SemanticResult> results;

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return results;
    }

    if (sqlite3_enable_load_extension(db, 1) != SQLITE_OK) {
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return results;
    }

    const char* vec_path = "C:\\Users\\niran\\Desktop\\FindX\\venv\\Lib\\site-packages\\sqlite_vec\\vec0";
    char* ext_err = nullptr;
    if (sqlite3_load_extension(db, vec_path, nullptr, &ext_err) != SQLITE_OK) {
        std::cerr << "Could not load vec extension: " << (ext_err ? ext_err : "unknown") << "\n";
        sqlite3_free(ext_err);
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return results;
    }

    // same CTE-based query structure we debugged in Python —
    // KNN resolves fully inside the CTE before any joins happen
    const char* sql =
        "WITH knn_matches AS ("
        "  SELECT chunk_id, distance"
        "  FROM chunk_vectors"
        "  WHERE embedding MATCH ?"
        "  AND k = ?"
        ")"
        "SELECT documents.id, documents.path, chunks.text, chunks.chunk_index, knn_matches.distance "
        "FROM knn_matches "
        "JOIN chunks ON chunks.id = knn_matches.chunk_id "
        "JOIN documents ON documents.id = chunks.doc_id "
        "ORDER BY knn_matches.distance;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "KNN query prepare failed: " << sqlite3_errmsg(db) << "\n";
        ec = std::make_error_code(std::errc::io_error);
        sqlite3_close(db);
        return results;
    }

    // bind the raw embedding bytes — SQLITE_STATIC is safe here because stmt
    // is fully consumed before query_embedding_bytes could ever go out of scope
    sqlite3_bind_blob(stmt, 1, query_embedding_bytes.data(),
                       static_cast<int>(query_embedding_bytes.size()), SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, top_k);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SemanticResult r;
        r.doc_id = static_cast<DocID>(sqlite3_column_int64(stmt, 0));

        const unsigned char* path_text = sqlite3_column_text(stmt, 1);
        r.path = std::filesystem::path(reinterpret_cast<const char*>(path_text));

        const unsigned char* chunk_text = sqlite3_column_text(stmt, 2);
        r.chunk_text = reinterpret_cast<const char*>(chunk_text);

        r.chunk_index = static_cast<std::size_t>(sqlite3_column_int(stmt, 3));
        r.distance = sqlite3_column_double(stmt, 4);

        results.push_back(r);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}