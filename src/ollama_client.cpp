#include "ollama_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <vector>

namespace {

constexpr const char* OLLAMA_HOST = "127.0.0.1";
constexpr int OLLAMA_PORT = 11434;

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

// shared connection setup — both streaming and non-streaming use the same socket init
SOCKET connect_to_ollama(std::error_code& ec) {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        ec = std::make_error_code(std::errc::io_error);
        return INVALID_SOCKET;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        ec = std::make_error_code(std::errc::io_error);
        WSACleanup();
        return INVALID_SOCKET;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(OLLAMA_PORT);
    inet_pton(AF_INET, OLLAMA_HOST, &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ec = std::make_error_code(std::errc::connection_refused);
        closesocket(sock);
        WSACleanup();
        return INVALID_SOCKET;
    }

    return sock;
}

bool send_request(SOCKET sock, const std::string& prompt, const std::string& model,
                   bool stream, std::error_code& ec) {
    std::string body = "{\"model\":\"" + model + "\","
                        "\"prompt\":\"" + escape_json(prompt) + "\","
                        "\"stream\":" + (stream ? "true" : "false") + ","
                        "\"options\":{\"num_predict\":512},"
                        "\"think\":false,"
                        "\"keep_alive\":-1}";

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
        return false;
    }
    return true;
}

// skip past HTTP headers, return everything after the blank line
std::string skip_http_headers(SOCKET sock, std::string& buffer) {
    while (true) {
        auto header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            return buffer.substr(header_end + 4);
        }
        char temp[4096];
        int bytes = recv(sock, temp, sizeof(temp), 0);
        if (bytes <= 0) return "";
        buffer.append(temp, bytes);
    }
}

} // namespace

std::string query_ollama(const std::string& prompt, const std::string& model, std::error_code& ec) {
    SOCKET sock = connect_to_ollama(ec);
    if (sock == INVALID_SOCKET) return "";

    if (!send_request(sock, prompt, model, false, ec)) {
        closesocket(sock);
        WSACleanup();
        return "";
    }

    std::string response;
    char buffer[4096];
    while (true) {
        int bytes = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        response.append(buffer, bytes);
    }

    closesocket(sock);
    WSACleanup();

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

void query_ollama_stream(const std::string& prompt, const std::string& model,
                          std::function<void(const std::string&)> on_token,
                          std::error_code& ec) {
    SOCKET sock = connect_to_ollama(ec);
    if (sock == INVALID_SOCKET) return;

    if (!send_request(sock, prompt, model, true, ec)) {
        closesocket(sock);
        WSACleanup();
        return;
    }

    // skip past HTTP headers to reach the streaming body
    std::string raw_buffer;
    std::string remaining = skip_http_headers(sock, raw_buffer);

    // process the stream line by line — each line is one JSON object with a single token
    while (true) {
        // check for complete lines already in our buffer
        auto newline_pos = remaining.find('\n');
        while (newline_pos != std::string::npos) {
            std::string line = remaining.substr(0, newline_pos);
            remaining = remaining.substr(newline_pos + 1);

            if (!line.empty()) {
                // extract and emit the token immediately
                std::string token = extract_json_string(line, "response");
                if (!token.empty()) {
                    on_token(token);
                }

                // check if this is the final line
                if (line.find("\"done\":true") != std::string::npos) {
                    closesocket(sock);
                    WSACleanup();
                    return;
                }
            }

            newline_pos = remaining.find('\n');
        }

        // need more data from the socket
        char temp[4096];
        int bytes = recv(sock, temp, sizeof(temp), 0);
        if (bytes <= 0) break;
        remaining.append(temp, bytes);
    }

    closesocket(sock);
    WSACleanup();
}