#include "crawler.h"

#include <iostream>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {

std::string format_time(std::filesystem::file_time_type ftime) {
    using namespace std::chrono;

    auto sctp = time_point_cast<system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());

    std::time_t tt = system_clock::to_time_t(sctp);
    std::tm* tm = std::localtime(&tt);

    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[1]) != "index") {
        std::cerr << "Usage: findx index <path>\n";
        return 1;
    }

    std::filesystem::path root = argv[2];

    std::error_code ec;
    std::vector<FileEntry> entries = crawl(root, ec);

    if (ec) {
        std::cerr << "Error crawling " << root << ": " << ec.message() << "\n";
        return 1;
    }

    for (const auto& entry : entries) {
        std::cout << entry.path.string()
                   << " | " << entry.size << " bytes"
                   << " | " << format_time(entry.last_write_time)
                   << "\n";
    }

    std::cout << "\nTotal files: " << entries.size() << "\n";

    return 0;
}