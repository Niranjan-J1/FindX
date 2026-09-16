#pragma once

#include <system_error>

bool register_context_menu(std::error_code& ec);
bool unregister_context_menu(std::error_code& ec);