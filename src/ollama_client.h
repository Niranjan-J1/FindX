#pragma once

#include <string>
#include <system_error>

std::string query_ollama(const std::string& prompt, std::error_code& ec);