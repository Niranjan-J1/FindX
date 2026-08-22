#pragma once

#include <string>
#include <unordered_map>
#include <cstddef>

using DocID = std::size_t;

class InvertedIndex {
public:
    void add_document(DocID id, const std::string& content);
    const std::unordered_map<DocID, int>* lookup(const std::string& term) const;

    std::size_t document_count() const;
    std::size_t document_length(DocID id) const;
    std::size_t total_token_count() const;

    const std::unordered_map<std::string, std::unordered_map<DocID, int>>& all_postings() const;
    void set_posting(const std::string& term, DocID id, int count);
    void set_document_length(DocID id, std::size_t length);

private:
    std::unordered_map<std::string, std::unordered_map<DocID, int>> index_;
    std::unordered_map<DocID, std::size_t> doc_lengths_;
};