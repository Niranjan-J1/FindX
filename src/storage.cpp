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
        "DROP TABLE IF EXISTS documents;";
    if (!exec_sql(db, drop_sql, ec)) { sqlite3_close(db); return false; }

    const char* create_sql =
        "CREATE TABLE documents ("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT NOT NULL UNIQUE,"
        "  size INTEGER NOT NULL,"
        "  mtime INTEGER NOT NULL,"
        "  token_count INTEGER NOT NULL"
        ");"
        "CREATE TABLE postings ("
        "  term TEXT NOT NULL,"
        "  doc_id INTEGER NOT NULL REFERENCES documents(id),"
        "  count INTEGER NOT NULL,"
        "  PRIMARY KEY (term, doc_id)"
        ");";
    if (!exec_sql(db, create_sql, ec)) { sqlite3_close(db); return false; }

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