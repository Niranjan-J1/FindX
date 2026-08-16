#pragma once

#include <string>
#include <unordered_map>
#include <cstddef>

using DocID = std::size_t;

class InvertedIndex {
public:
    void add_document(DocID id, const std::string& content);
    const std::unordered_map<DocID, int>* lookup(const std::string& term) const;

private:
    std::unordered_map<std::string, std::unordered_map<DocID, int>> index_;
};