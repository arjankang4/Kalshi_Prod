cmake_minimum_required(VERSION 3.22)
project(kalshi_prod CXX)

# C++17, not 20: g++ 11.5 has no <format> and no <chrono> tzdb (that landed in GCC 13).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
# -O2, never -O3/-Ofast: -O3 enables vectorisation passes that can reshape the reduction
# loops in atmvol.parity_forward and theo.bracket_fair. This workload prices ~400 markets
# every quote cycle and is nowhere near compute-bound.
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELEASE        "-O2")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Dependencies.cmake)
include(cmake/FloatingPoint.cmake)
include(cmake/Warnings.cmake)

# The Python reference tree. Kalshi_Prod deliberately has no var/ of its own: secrets and
# model artifacts are read from here, unchanged. Copying them is how two trees diverge.
set(KALSHI_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/../Kalshi" CACHE PATH
    "Python reference tree (default: a sibling checkout of the Kalshi repo)")
add_compile_definitions(KP_KALSHI_ROOT_DEFAULT="${KALSHI_ROOT_DEFAULT}")
# The frozen copy of the Python CODE the vectors are generated from (see ref/). Model artifacts and
# secrets still come from KALSHI_ROOT; only the code is pinned, because the code is what moves.
set(KP_PYTHON "python3" CACHE FILEPATH
    "The reference interpreter: one with the Kalshi repo's dependencies (websocket-client, numpy)")
set(KP_REF_DEFAULT "${CMAKE_SOURCE_DIR}/ref/current" CACHE PATH "Frozen Python reference snapshot")

add_subdirectory(src)
add_subdirectory(apps)
enable_testing()
add_subdirectory(tests)
