cmake_minimum_required(VERSION 3.10)
project(proxy C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")

add_executable(proxy ${PROJECT_SOURCE_DIR}/main.c)