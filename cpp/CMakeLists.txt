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

# Must come after the option above, since the whole file is a no-op when
# HC_BUILD_APP is OFF. Defines hc::opencv when it is ON.
include(cmake/FetchOpenCV.cmake)

add_subdirectory(tools)

# Added bottom-up, in dependency order, so each hc_assert_does_not_link check
# sees the targets below it already defined.
#
#   base/    hc_base        primitives: geometry, image types, metrics, frame
#                           sources. No ONNX, no OS, no OpenCV.
#   vision/  hc_vision      the standalone hand-detection module: anchors,
#                           palm decode, ROI, warp, association, features.
#            hc_vision_ort  its ONNX Runtime backend, the only ORT consumer.
#
#   control/ hc_control    the logic layer: gesture recognition, commands,
#                          bindings, mapping. Portable, and the only place
#                          those decisions are made.
add_subdirectory(base)
add_subdirectory(vision)
add_subdirectory(control)

# platform/ is the per-system layer and the only one that links OpenCV, so it
# is guarded: a core-only configure (-DHC_BUILD_APP=OFF) builds no OpenCV and
# links none.
if(HC_BUILD_APP)
  add_subdirectory(platform)
endif()

# app/ is NOT guarded. It declares hc_cli, which is portable and whose tests
# should run in the fast core-only configure; the hand_controller executable
# inside it guards itself on HC_BUILD_APP.
add_subdirectory(app)

enable_testing()
add_subdirectory(tests)

# Last, deliberately. Every hc_assert_* call above only registered an
# assertion; this is where they run, now that the link graph is final and no
# later target_link_libraries can sneak in behind them.
hc_run_layer_checks()
