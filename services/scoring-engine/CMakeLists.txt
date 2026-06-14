cmake_minimum_required(VERSION 3.20)
project(scoring-engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(hiredis CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cppkafka CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)

add_executable(scoring-engine src/main.cpp)

target_link_libraries(scoring-engine PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    CppKafka::cppkafka
    libpqxx::pqxx
    redis++::redis++_static
    hiredis::hiredis
)
