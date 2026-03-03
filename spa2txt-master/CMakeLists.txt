cmake_minimum_required(VERSION 3.15)

cmake_policy(SET CMP0091 NEW)

project("spa2txt" C)

if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_C_STANDARD 99)
add_library(spa spa.c)
add_executable(spa2txt main.c)
target_link_libraries(spa2txt spa)