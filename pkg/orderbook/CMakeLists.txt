cmake_minimum_required(VERSION 3.14)
project(orderbook_lib CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# nlohmann_json is provided by vcpkg
find_package(nlohmann_json CONFIG REQUIRED)

add_library(orderbook
    src/orderbook.cpp
)

target_include_directories(orderbook PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(orderbook PUBLIC nlohmann_json::nlohmann_json)

# We might need threads
find_package(Threads REQUIRED)
target_link_libraries(orderbook PRIVATE Threads::Threads)
