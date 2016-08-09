cmake_minimum_required(VERSION 2.8)
project(abuv)

include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(${PROJECT_NAME}
    src/abuv.c
    src/http_parser.c
    src/http_parser.h
)
