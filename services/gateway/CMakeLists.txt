cmake_minimum_required(VERSION 3.16)
project(gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(gateway src/main.cpp)

target_link_libraries(gateway PRIVATE 
    Drogon::Drogon
    nlohmann_json::nlohmann_json
)
