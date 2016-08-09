cmake_minimum_required(VERSION 2.8)
project(abuv)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
find_package(LibUV REQUIRED)

include_directories(${LIBUV_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(${PROJECT_NAME}
    src/abuv.c
    src/getopt.c
    src/getopt.h
    src/http_parser.c
    src/http_parser.h
    src/stopwatch.c
    src/stopwatch.h
)

target_link_libraries(${PROJECT_NAME}
    ${LIBUV_LIBRARIES}
)
