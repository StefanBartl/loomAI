cmake_minimum_required(VERSION 3.20)
project(loomai CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Header-only third-party deps, vendored into third_party/ (see
# docs/Guides/setup-guide.md, Phase 2 -- gitignored, fetch locally).
add_library(third_party_headers INTERFACE)
target_include_directories(third_party_headers INTERFACE third_party)

add_executable(loomai
    src/main.cpp
    src/ollama_client.cpp
)
target_include_directories(loomai PRIVATE src)
target_link_libraries(loomai PRIVATE third_party_headers)

if(WIN32)
    target_link_libraries(loomai PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(loomai PRIVATE Threads::Threads)
endif()
