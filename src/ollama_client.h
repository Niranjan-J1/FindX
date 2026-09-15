#pragma once
#include <functional>
#include <string>
#include <system_error>

void query_ollama_stream(const std::string& prompt, const std::string& model,
                          std::function<void(const std::string&)> on_token,
                          std::error_code& ec);