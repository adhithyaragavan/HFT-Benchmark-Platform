cmake_minimum_required(VERSION 3.20)
project(trading-platform-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable vcpkg manifest mode if vcpkg toolchain is provided
# (e.g., via -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake)

# Subdirectories for each service
add_subdirectory(gateway)
add_subdirectory(bot-orchestrator)
add_subdirectory(bot-worker)
add_subdirectory(submission-svc)
add_subdirectory(scoring-engine)
add_subdirectory(leaderboard-svc)
add_subdirectory(sandbox-mgr)
