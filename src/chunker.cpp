#include "chunker.h"

#include <cctype>
#include <algorithm>

namespace {
    constexpr std::size_t CHUNK_SIZE = 200;
    constexpr std::size_t CHUNK_OVERLAP = 40;
}

std::vector<std::string> chunk_text(const std::string& content) {
    std::vector<std::string> words;
    std::string current;

    for (unsigned char c : content) {
        if (std::isspace(c)) {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += static_cast<char>(c);
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }

    std::vector<std::string> chunks;
    if (words.empty()) {
        return chunks;
    }

    std::size_t step = CHUNK_SIZE - CHUNK_OVERLAP;

    for (std::size_t start = 0; start < words.size(); start += step) {
        std::size_t end = std::min(start + CHUNK_SIZE, words.size());

        std::string chunk;
        for (std::size_t i = start; i < end; ++i) {
            if (i > start) {
                chunk += ' ';
            }
            chunk += words[i];
        }
        chunks.push_back(std::move(chunk));

        if (end == words.size()) {
            break;
        }
    }

    return chunks;
}