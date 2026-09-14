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

const std::unordered_map<std::string, std::unordered_map<DocID, int>>& InvertedIndex::all_postings() const {
    return index_;
}

void InvertedIndex::set_posting(const std::string& term, DocID id, int count) {
    index_[term][id] = count;
}

void InvertedIndex::set_document_length(DocID id, std::size_t length) {
    doc_lengths_[id] = length;
}

std::vector<HybridResult> merge_hybrid(
    const std::vector<ScoredDocument>& bm25_results,
    const std::vector<DocumentRecord>& documents,
    const std::vector<SemanticResult>& semantic_results,
    std::size_t max_results)
{
    constexpr double K = 60.0;

    // accumulate RRF scores per doc_id
    std::unordered_map<DocID, double> rrf_scores;
    std::unordered_map<DocID, std::filesystem::path> paths;
    std::unordered_map<DocID, std::string> previews;

    // BM25 results are already sorted by score descending — rank is just position
    for (std::size_t rank = 0; rank < bm25_results.size(); ++rank) {
        DocID id = bm25_results[rank].id;
        rrf_scores[id] += 1.0 / (K + static_cast<double>(rank + 1));
        if (id < documents.size()) {
            paths[id] = documents[id].path;
        }
    }

    // semantic results are already sorted by distance ascending — rank is just position
    for (std::size_t rank = 0; rank < semantic_results.size(); ++rank) {
        DocID id = semantic_results[rank].doc_id;
        rrf_scores[id] += 1.0 / (K + static_cast<double>(rank + 1));
        paths[id] = semantic_results[rank].path;

        // keep the best (first seen) chunk preview per document
        if (previews.find(id) == previews.end()) {
            std::string preview = semantic_results[rank].chunk_text.substr(0, 120);
            if (semantic_results[rank].chunk_text.size() > 120) {
                preview += "...";
            }
            previews[id] = preview;
        }
    }

    // collect and sort by RRF score descending
    std::vector<HybridResult> results;
    for (const auto& [id, score] : rrf_scores) {
        HybridResult r;
        r.doc_id = id;
        r.path = paths[id];
        r.chunk_preview = previews.count(id) ? previews[id] : "";
        r.rrf_score = score;
        results.push_back(r);
    }

    std::sort(results.begin(), results.end(),
        [](const HybridResult& a, const HybridResult& b) {
            return a.rrf_score > b.rrf_score;
        });

    if (results.size() > max_results) {
        results.resize(max_results);
    }

    return results;
}