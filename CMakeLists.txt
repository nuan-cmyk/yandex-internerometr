cmake_minimum_required(VERSION 3.20)
project(internetometer_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(internetometer
    src/main.cpp
    src/yandex_client.cpp
    src/tui.cpp
)

target_include_directories(internetometer PRIVATE src)
target_link_libraries(internetometer PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)

if (WIN32)
    target_link_libraries(internetometer PRIVATE ws2_32)
endif()
