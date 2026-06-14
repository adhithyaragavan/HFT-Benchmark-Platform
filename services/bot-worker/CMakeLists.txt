cmake_minimum_required(VERSION 3.14)
project(bot_worker CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required packages
find_package(nlohmann_json REQUIRED)
find_package(cppkafka REQUIRED)
find_package(httplib REQUIRED)

add_executable(bot_worker
    src/main.cpp
    src/bot.cpp
    src/sender.cpp
    src/reporter.cpp
    src/worker.cpp
)

target_link_libraries(bot_worker
    PRIVATE
    nlohmann_json::nlohmann_json
    CppKafka::cppkafka
    httplib::httplib
    pthread
)
