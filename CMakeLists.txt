cmake_minimum_required(VERSION 3.20)

project(findx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(findx
    src/main.cpp
    src/crawler.cpp
    src/reader.cpp
    src/tokenizer.cpp
    src/index.cpp
    src/ranker.cpp
    src/storage.cpp
)

find_package(SQLite3)

if(SQLite3_FOUND)
    message(STATUS "Linking SQLite3 via find_package")
    target_link_libraries(findx PRIVATE SQLite::SQLite3)
else()
    message(STATUS "find_package could not locate SQLite3 — falling back to manual MSYS2 paths")
    target_include_directories(findx PRIVATE "C:/msys64/mingw64/include")
    target_link_directories(findx PRIVATE "C:/msys64/mingw64/lib")
    target_link_libraries(findx PRIVATE sqlite3)
endif()