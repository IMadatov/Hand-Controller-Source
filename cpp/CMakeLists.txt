cmake_minimum_required(VERSION 3.28)

project(hand_controller_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/HcAssertNoForbiddenDeps.cmake)

# ONNX Runtime and the model fetch must stay unconditional: Task 8's ONNX
# tests build with -DHC_BUILD_APP=OFF and still need HC_PALM_MODEL /
# HC_LANDMARK_MODEL and the ORT headers. Only OpenCV (added in Task 12) is
# guarded by HC_BUILD_APP.
include(cmake/FetchOnnxRuntime.cmake)
include(cmake/FetchModels.cmake)

# When OFF, OpenCV and the app target (added in Task 12) are neither fetched
# nor built. Tasks 2-11 run their test cycles with this OFF, since OpenCV
# from source is a slow first configure and nothing before Task 12 needs it.
option(HC_BUILD_APP "Build the OpenCV-based application target" ON)

add_subdirectory(tools)
add_subdirectory(core)

enable_testing()
add_subdirectory(tests)
