cmake_minimum_required(VERSION 3.21)

project(beebcrunch C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The parse is an order of magnitude slower unoptimized; default to Release
# so the post-build test run stays fast.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_TESTS "Build unit tests into beebcrunch and run them post-build" ON)

set(RICHC_CORE_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(extern/richc)

add_subdirectory(src)
