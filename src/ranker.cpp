#include "ranker.h"

#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace {
    constexpr double K1 = 1.2;
    constexpr double B = 0.75;
}

std::vector<ScoredDocument> rank_bm25(const InvertedIndex& index, const std::vector<std::string>& query_terms) {
    std::vector<ScoredDocument> results;

    std::size_t N = index.document_count();
    if (N == 0) {
        return results;
    }

    double avg_doc_length = static_cast<double>(index.total_token_count()) / static_cast<double>(N);

    std::unordered_map<DocID, double> scores;

    for (const auto& term : query_terms) {
        const auto* matches = index.lookup(term);
        if (!matches) {
            continue;
        }

        double df = static_cast<double>(matches->size());
        double idf = std::log((static_cast<double>(N) - df + 0.5) / (df + 0.5) + 1.0);

        for (const auto& [id, tf_int] : *matches) {
            double tf = static_cast<double>(tf_int);
            double doc_length = static_cast<double>(index.document_length(id));

            double length_ratio = (avg_doc_length > 0.0) ? (doc_length / avg_doc_length) : 1.0;
            double length_norm = (1.0 - B + B * length_ratio);

            double tf_component = (tf * (K1 + 1.0)) / (tf + K1 * length_norm);

            scores[id] += idf * tf_component;
        }
    }

    results.reserve(scores.size());
    for (const auto& [id, score] : scores) {
        results.push_back(ScoredDocument{ id, score });
    }

    std::sort(results.begin(), results.end(),
        [](const ScoredDocument& a, const ScoredDocument& b) {
            return a.score > b.score;
        });

    return results;
}