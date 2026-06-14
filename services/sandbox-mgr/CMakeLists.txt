cmake_minimum_required(VERSION 3.14)
project(sandbox-mgr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(httplib CONFIG REQUIRED)

file(GLOB SOURCES "src/*.cpp")

add_executable(sandbox-mgr ${SOURCES})

target_link_libraries(sandbox-mgr PRIVATE 
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    httplib::httplib
)
