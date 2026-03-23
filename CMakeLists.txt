cmake_minimum_required(VERSION 3.16)
project(konacode LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# unified output dirs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)

# build core
add_subdirectory(core)
