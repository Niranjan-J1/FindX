#include "embed_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>

namespace {

constexpr const char* SERVER_IP = "127.0.0.1";
constexpr int SERVER_PORT = 8765;

}

std::vector<float> get_query_embedding(const std::string& query, std::error_code& ec) {
    std::vector<float> result;

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        ec = std::make_error_code(std::errc::io_error);
        return result;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        ec = std::make_error_code(std::errc::io_error);
        WSACleanup();
        return result;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        ec = std::make_error_code(std::errc::connection_refused);
        closesocket(sock);
        WSACleanup();
        return result;
    }

    std::string message = query + "\n";
    if (send(sock, message.c_str(), static_cast<int>(message.size()), 0) == SOCKET_ERROR) {
        ec = std::make_error_code(std::errc::io_error);
        closesocket(sock);
        WSACleanup();
        return result;
    }

    // same "loop until the delimiter shows up" logic as the Python server —
    // a single recv() is not guaranteed to contain the full response
    std::string response;
    char buffer[4096];
    while (response.empty() || response.back() != '\n') {
        int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            break;
        }
        response.append(buffer, bytes_received);
    }

    closesocket(sock);
    WSACleanup();

    if (response.empty()) {
        ec = std::make_error_code(std::errc::io_error);
        return result;
    }

    // parse the comma-separated floats back into a real vector
    std::istringstream iss(response);
    std::string token;
    while (std::getline(iss, token, ',')) {
        try {
            result.push_back(std::stof(token));
        } catch (...) {
            ec = std::make_error_code(std::errc::io_error);
            return {};
        }
    }

    return result;
}