

cmake_minimum_required(VERSION 3.10)

project(eduterm C)

set(CMAKE_C_STANDARD 99)

add_executable(eduterm eduterm.c)
target_link_libraries(eduterm X11)
