#pragma once

#include <string>
#include <vector>
#include <system_error>

std::vector<float> get_query_embedding(const std::string& query, std::error_code& ec);

std::vector<char> serialize_float_vector(const std::vector<float>& vec);