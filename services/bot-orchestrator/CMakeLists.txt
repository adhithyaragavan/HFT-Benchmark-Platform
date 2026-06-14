cmake_minimum_required(VERSION 3.16)
project(bot-orchestrator CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(cppkafka REQUIRED)

add_executable(bot-orchestrator
    src/main.cpp
)

target_link_libraries(bot-orchestrator
    PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    CppKafka::cppkafka
)
