#pragma once

#include "index.h"

#include <vector>
#include <string>

struct ScoredDocument{
    DocID id;
    double score;
};

std::vector<ScoredDocument> rank_bm25(const InvertedIndex& index, const std::vector<std::string>& query_terms );
