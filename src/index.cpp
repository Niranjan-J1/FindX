#include "index.h"
#include "tokenizer.h"

void InvertedIndex::add_document(DocID id, const std::string& content) {
    std::vector<std::string> tokens = tokenize(content);

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