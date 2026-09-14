#include "ollama_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <vector>

namespace {

constexpr const char* OLLAMA_HOST = "127.0.0.1";
constexpr int OLLAMA_PORT = 11434;

// minimal JSON string escaping — handles the characters that would break a JSON value
std::string escape_json(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

// extract the value of a given key from a flat JSON object — intentionally simple,
// not a real JSON parser, just enough for Ollama's response format
std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '"') { result += '"'; pos += 2; continue; }
            if (next == 'n') { result += '\n'; pos += 2; continue; }
            if (next == 't') { result += '\t'; pos += 2; continue; }
            if (next == '\\') { result += '\\'; pos += 2; continue; }
        }
        result += json[pos];
        ++pos;
    }
    return result;
}

} // namespace

std::string query_ollama(const std::string& prompt, std::error_code& ec) {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        ec = std::make_error_code(std::errc::io_error);
        return "";
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        ec = std::make_error_code(std::errc::io_error);
        WSACleanup();
        return "";
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(OLLAMA_PORT);
    inet_pton(AF_INET, OLLAMA_HOST, &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ec = std::make_error_code(std::errc::connection_refused);
        closesocket(sock);
        WSACleanup();
        return "";
    }

    // build the JSON request body — stream:false gets the complete response in one piece,
    // think:false suppresses qwen3's chain-of-thought output
    std::string body = "{\"model\":\"qwen3:8b\","
                        "\"prompt\":\"" + escape_json(prompt) + "\","
                        "\"stream\":false,"
                        "\"options\":{\"num_predict\":512},"
                        "\"think\":false}";

    // construct a minimal valid HTTP POST request
    std::ostringstream request;
    request << "POST /api/generate HTTP/1.1\r\n"
            << "Host: 127.0.0.1:11434\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;

    std::string req_str = request.str();
    if (send(sock, req_str.c_str(), static_cast<int>(req_str.size()), 0) == SOCKET_ERROR) {
        ec = std::make_error_code(std::errc::io_error);
        closesocket(sock);
        WSACleanup();
        return "";
    }

    // read the full HTTP response — loop until the connection closes
    std::string response;
    char buffer[4096];
    while (true) {
        int bytes = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        response.append(buffer, bytes);
    }

    closesocket(sock);
    WSACleanup();

    // separate HTTP headers from body — they're divided by a blank line
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        ec = std::make_error_code(std::errc::io_error);
        return "";
    }
    std::string response_body = response.substr(body_start + 4);

    std::string answer = extract_json_string(response_body, "response");
    if (answer.empty()) {
        ec = std::make_error_code(std::errc::io_error);
    }

    return answer;
}