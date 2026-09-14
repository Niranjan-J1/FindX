#pragma once

#include "index.h"

#include <vector>
#include <string>
#include "storage.h"

struct ScoredDocument{
    DocID id;
    double score;
};


struct HybridResult {
    DocID doc_id;
    std::filesystem::path path;
    std::string chunk_preview;   // from semantic results, empty if only BM25 matched
    double rrf_score;
};

std::vector<HybridResult> merge_hybrid(
    const std::vector<ScoredDocument>& bm25_results,
    const std::vector<DocumentRecord>& documents,
    const std::vector<SemanticResult>& semantic_results,
    std::size_t max_results);

std::vector<ScoredDocument> rank_bm25(const InvertedIndex& index, const std::vector<std::string>& query_terms );
 

