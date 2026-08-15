#include "tokenizer.h"

#include <cctype>

namespace {

bool is_word_char(unsigned char c) {
    return std::isalnum(c) || c == '_';
}

} // namespace

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;

    for (unsigned char c : text) {
        if (is_word_char(c)) {
            current += static_cast<char>(std::tolower(c));
        } else {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        }
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}