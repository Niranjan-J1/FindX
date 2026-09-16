#include "registry.h"

#include <windows.h>
#include <string>

namespace {

std::string get_exe_path() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return std::string(buffer);
}

bool set_registry_key(HKEY root, const std::string& path, const std::string& value,
                       std::error_code& ec) {
    HKEY key;
    LONG result = RegCreateKeyExA(root, path.c_str(), 0, nullptr,
                                   REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    result = RegSetValueExA(key, nullptr, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>(value.size() + 1));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    return true;
}

bool delete_registry_tree(HKEY root, const std::string& path) {
    return RegDeleteTreeA(root, path.c_str()) == ERROR_SUCCESS;
}

} // namespace

bool register_context_menu(std::error_code& ec) {
    std::string exe = get_exe_path();

    std::string index_key = "Software\\Classes\\Directory\\shell\\FindX_Index";
    std::string index_cmd_key = index_key + "\\command";
    std::string index_label = "Index with FindX";
    std::string index_command = "cmd /k \"\"" + exe + "\" index \"%V\" && echo. && echo Press any key to close... && pause >nul\"";
    if (!set_registry_key(HKEY_CURRENT_USER, index_key, index_label, ec)) return false;
    if (!set_registry_key(HKEY_CURRENT_USER, index_cmd_key, index_command, ec)) return false;

    std::string find_key = "Software\\Classes\\Directory\\shell\\FindX_Find";
    std::string find_cmd_key = find_key + "\\command";
    std::string find_label = "Find with FindX";
    std::string find_command = "\"" + exe + "\" app --folder \"%V\"";

    if (!set_registry_key(HKEY_CURRENT_USER, find_key, find_label, ec)) return false;
    if (!set_registry_key(HKEY_CURRENT_USER, find_cmd_key, find_command, ec)) return false;

    return true;
}

bool unregister_context_menu(std::error_code& ec) {
    bool ok = true;
    if (!delete_registry_tree(HKEY_CURRENT_USER,
         "Software\\Classes\\Directory\\shell\\FindX_Index")) {
        ok = false;
    }
    if (!delete_registry_tree(HKEY_CURRENT_USER,
         "Software\\Classes\\Directory\\shell\\FindX_Find")) {
        ok = false;
    }
    if (!ok) {
        ec = std::make_error_code(std::errc::io_error);
    }
    return ok;
}