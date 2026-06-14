cmake_minimum_required(VERSION 3.20)
project(cpp-exchange CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use httplib from vcpkg (already declared in services/vcpkg.json)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Add executable
add_executable(cpp-exchange src/main.cpp)

# Link orderbook and dependencies
target_link_libraries(cpp-exchange PRIVATE
    orderbook
    nlohmann_json::nlohmann_json
    httplib::httplib
)
