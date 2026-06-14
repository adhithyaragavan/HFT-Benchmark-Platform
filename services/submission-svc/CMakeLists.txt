cmake_minimum_required(VERSION 3.14)
project(submission_svc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(libpqxx REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS s3)
find_package(hiredis CONFIG REQUIRED)
find_package(redis++ REQUIRED)

add_executable(submission_svc src/main.cpp)

target_link_libraries(submission_svc
    PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    ${AWSSDK_LINK_LIBRARIES}
    redis++::redis++_static
)
