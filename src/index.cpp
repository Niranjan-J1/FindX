#include "index.h"
#include "tokenizer.h"

#include <vector>

void InvertedIndex::add_document(DocID id, const std::string& content) {
    std::vector<std::string> tokens = tokenize(content);

    doc_lengths_[id] = tokens.size();

    for (const auto& term : tokens) {
        index_[term][id]++;
    }
}

const std::unordered_map<DocID, int>* InvertedIndex::lookup(const std::string& term) const {
    auto it = index_.find(term);
    if (it == index_.end()) {
        return nullptr;
    }
    return &(it->second);
}

std::size_t InvertedIndex::document_count() const {
    return doc_lengths_.size();
}

std::size_t InvertedIndex::document_length(DocID id) const {
    auto it = doc_lengths_.find(id);
    if (it == doc_lengths_.end()) {
        return 0;
    }
    return it->second;
}

std::size_t InvertedIndex::total_token_count() const {
    std::size_t total = 0;
    for (const auto& [id, length] : doc_lengths_) {
        total += length;
    }
    return total;
}