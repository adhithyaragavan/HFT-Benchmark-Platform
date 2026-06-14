cmake_minimum_required(VERSION 3.20)
project(trading-platform CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pre-load hiredis before any subdirectory tries to find it via drogon or redis++.
# This prevents the drogon-bundled Findhiredis.cmake from hijacking the lookup.
find_package(hiredis CONFIG QUIET)

# Subdirectories for shared packages, microservices, and contestant templates
add_subdirectory(pkg/orderbook)
add_subdirectory(services)
add_subdirectory(templates/cpp-exchange)
