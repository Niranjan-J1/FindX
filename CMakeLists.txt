cmake_minimum_required(VERSION 3.20)

project(findx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(findx
    src/main.cpp
    src/crawler.cpp
    src/reader.cpp
    src/tokenizer.cpp
    src/index.cpp
    src/ranker.cpp
)