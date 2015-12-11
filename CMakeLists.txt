cmake_minimum_required(VERSION 2.8)
project(mtou C)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -std=gnu11 -D__USE_POSIX199309")

set(SOURCE_FILES main.c)
add_executable(mtou ${SOURCE_FILES})
target_link_libraries(mtou -lpthread)
