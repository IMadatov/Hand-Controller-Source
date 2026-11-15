include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Thin wrapper so every test file in this directory is registered the same
# way: hc_add_test(<name> <sources...> LIBS <targets...>) builds <name> from
# <sources...>, links GoogleTest's main plus <targets...> (the target(s)
# under test), and discovers its cases via ctest. One call per test binary;
# later tasks each add one.
function(hc_add_test name)
  cmake_parse_arguments(HC_ADD_TEST "" "" "LIBS" ${ARGN})
  add_executable(${name} ${HC_ADD_TEST_UNPARSED_ARGUMENTS})
  target_link_libraries(${name} PRIVATE GTest::gtest_main ${HC_ADD_TEST_LIBS})
  gtest_discover_tests(${name})
endfunction()

# Every test links the NARROWEST target that satisfies it, never a convenient
# superset. That is what makes the module split verifiable rather than
# decorative: if a unit quietly grows a dependency on a higher layer, its own
# test stops linking, and it does so here rather than in a code review.

# hc_base — the primitives. These link no inference runtime at all.
hc_add_test(test_geometry test_geometry.cpp LIBS hc_base)
hc_add_test(test_metrics test_metrics.cpp LIBS hc_base)
hc_add_test(test_frame_source test_frame_source.cpp LIBS hc_base)

# hc_vision — the detection algorithms. Linking these against hc_vision
# rather than hc_vision_ort is the standing proof that they need no ONNX
# Runtime, which is the precondition for the WebAssembly port.
hc_add_test(test_warp test_warp.cpp LIBS hc_vision)
hc_add_test(test_anchors test_anchors.cpp LIBS hc_vision)
hc_add_test(test_palm_decode test_palm_decode.cpp LIBS hc_vision)
hc_add_test(test_roi test_roi.cpp LIBS hc_vision)
hc_add_test(test_association test_association.cpp LIBS hc_vision)
hc_add_test(test_hand_features test_hand_features.cpp LIBS hc_vision)

# hc_control — the logic layer. These tests drive synthetic landmark poses
# through the state machine, so they need no camera, no display and no model.
hc_add_test(test_gesture_recognizer test_gesture_recognizer.cpp LIBS hc_control)

hc_add_test(test_bindings test_bindings.cpp LIBS hc_control)
# The path is injected rather than hardcoded so the test can check that the
# text CMake compiled into hc_control is still the text in the shipped file.
target_compile_definitions(test_bindings PRIVATE
  HC_DEFAULT_BINDINGS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/default_bindings.txt"
)

hc_add_test(test_action_mapper test_action_mapper.cpp LIBS hc_control)
hc_add_test(test_output_sink test_output_sink.cpp LIBS hc_control)
hc_add_test(test_queued_sink test_queued_sink.cpp LIBS hc_control)

# The whole chain joined up -- frame source, tracker, recognizer, mapper,
# sink -- driven through a SCRIPTED tracker, so it needs no model, no camera
# and no display, and a failure names the wiring rather than the inference.
hc_add_test(test_controller test_controller.cpp LIBS hc_control)
target_include_directories(test_controller PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# hc_vision_ort — the ONNX Runtime backend, and the only tests that load
# real model files.
hc_add_test(test_hand_tracker test_hand_tracker.cpp LIBS hc_vision_ort)
# The tracker tests are the only ones that touch real model files and the
# committed image fixture, so the paths are injected here rather than being
# hardcoded in the test source. HC_PALM_MODEL / HC_LANDMARK_MODEL are the
# absolute, fetch-time paths set by cmake/FetchModels.cmake.
target_compile_definitions(test_hand_tracker PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
  HC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(test_hand_tracker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Task 10's two-hand tests need exactly the same injected paths: the same
# models, and the same committed fixture (which they composite into
# multi-hand frames via fixture_image.hpp).
hc_add_test(test_hand_tracker_two test_hand_tracker_two.cpp LIBS hc_vision_ort)
target_compile_definitions(test_hand_tracker_two PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
  HC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(test_hand_tracker_two PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# hc_cli — argument parsing only, and deliberately OUTSIDE the HC_BUILD_APP
# guard below. ParseCli links nothing but the standard library, so there is no
# reason its tests should wait on an OpenCV build; they run in the core-only
# configure with the rest of the portable code.
#
# main() itself is not linked here -- ParseCli lives in its own translation
# unit precisely so the tests can reach it without pulling in a second main().
hc_add_test(test_cli test_cli.cpp LIBS hc_cli)
# ReadTextFile needs a file it can actually open; the shipped configuration is
# the one file guaranteed to be there, and its path comes from the build rather
# than from a guess about the working directory.
target_compile_definitions(test_cli PRIVATE
  HC_DEFAULT_BINDINGS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/default_bindings.txt"
)

# Tests for the platform layer. Guarded, because hc_platform and OpenCV only
# exist when HC_BUILD_APP is ON; with it OFF this whole block is skipped and
# the suite stays buildable with no OpenCV anywhere.
if(HC_BUILD_APP)
  # A scratch directory under the build tree for tests that generate media
  # files, so nothing is written into the source tree.
  set(HC_TEST_TMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/tmp")
  file(MAKE_DIRECTORY "${HC_TEST_TMP_DIR}")

  hc_add_test(test_opencv_frame_source test_opencv_frame_source.cpp LIBS hc_platform)
  target_compile_definitions(test_opencv_frame_source PRIVATE
    HC_TEST_TMP_DIR="${HC_TEST_TMP_DIR}"
  )

  # Only DrawObservations is covered. Overlay::Render needs a display and is
  # exercised by Task 14's manual run instead.
  hc_add_test(test_overlay test_overlay.cpp LIBS hc_platform)

  # The uinput sink, Linux only. Every case here reads the virtual device's
  # own evdev node back, so these are measurements rather than assertions that
  # a write returned -- the distinction XTest's silent failure taught.
  #
  # They SKIP when /dev/uinput is not accessible, which is the default on a
  # fresh machine. A skipped run means UNVERIFIED, not passed; to run them for
  # real after granting access:
  #
  #   sudo -u "$USER" -g input ./cpp/build-app/bin/test_uinput_sink
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    hc_add_test(test_uinput_sink test_uinput_sink.cpp LIBS hc_platform)
  endif()
endif()
