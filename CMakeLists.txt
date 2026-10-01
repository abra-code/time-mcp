cmake_minimum_required(VERSION 3.16)

# Nothing here needs a recent macOS; any release that runs arm64 will do.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

project(time-mcp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# yyjson, vendored. The server reads and writes whole documents and nothing
# else, so the optional parts are left out (about half of the library).
add_library(yyjson STATIC vendor/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC vendor/yyjson)
target_compile_definitions(yyjson PUBLIC
    YYJSON_DISABLE_UTILS=1
    YYJSON_DISABLE_INCR_READER=1
    YYJSON_DISABLE_NON_STANDARD=1
    YYJSON_DISABLE_FAST_FP_CONV=1)

# Everything but main(), so the test tools link the same code.
add_library(timemcp STATIC
    src/McpStdio.cpp)
target_include_directories(timemcp PUBLIC src)
target_link_libraries(timemcp PUBLIC yyjson)
target_compile_options(timemcp PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fno-exceptions -fno-rtti)

add_executable(time-mcp src/main.cpp)
target_link_libraries(time-mcp PRIVATE timemcp)
target_compile_definitions(time-mcp PRIVATE TIME_MCP_VERSION="${PROJECT_VERSION}")
if(APPLE)
    target_link_options(time-mcp PRIVATE -Wl,-dead_strip)
else()
    target_compile_options(timemcp PUBLIC -ffunction-sections -fdata-sections)
    target_link_options(time-mcp PRIVATE -Wl,--gc-sections)
endif()

install(TARGETS time-mcp RUNTIME DESTINATION bin)
