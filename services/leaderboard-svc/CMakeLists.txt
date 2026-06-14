cmake_minimum_required(VERSION 3.14)
project(leaderboard-svc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(hiredis CONFIG REQUIRED)
find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(redis++ REQUIRED)

add_executable(${PROJECT_NAME} src/main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    redis++::redis++_static
)
