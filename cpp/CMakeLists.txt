cmake_minimum_required(VERSION 3.28)

project(hand_controller_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/FetchOnnxRuntime.cmake)
include(cmake/FetchModels.cmake)

add_subdirectory(tools)
