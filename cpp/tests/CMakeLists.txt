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

hc_add_test(test_geometry test_geometry.cpp LIBS hc_core)
hc_add_test(test_metrics test_metrics.cpp LIBS hc_core)
hc_add_test(test_warp test_warp.cpp LIBS hc_core)
hc_add_test(test_anchors test_anchors.cpp LIBS hc_core)
hc_add_test(test_palm_decode test_palm_decode.cpp LIBS hc_core)
hc_add_test(test_roi test_roi.cpp LIBS hc_core)
hc_add_test(test_association test_association.cpp LIBS hc_core)
hc_add_test(test_frame_source test_frame_source.cpp LIBS hc_core)

hc_add_test(test_hand_tracker test_hand_tracker.cpp LIBS hc_core)
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
hc_add_test(test_hand_tracker_two test_hand_tracker_two.cpp LIBS hc_core)
target_compile_definitions(test_hand_tracker_two PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
  HC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(test_hand_tracker_two PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Tests for the app layer. Guarded, because the app layer and OpenCV only
# exist when HC_BUILD_APP is ON; with it OFF this whole block is skipped and
# the suite stays buildable with no OpenCV anywhere.
if(HC_BUILD_APP)
  # A scratch directory under the build tree for tests that generate media
  # files, so nothing is written into the source tree.
  set(HC_TEST_TMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/tmp")
  file(MAKE_DIRECTORY "${HC_TEST_TMP_DIR}")

  hc_add_test(test_opencv_frame_source test_opencv_frame_source.cpp LIBS hc_app)
  target_compile_definitions(test_opencv_frame_source PRIVATE
    HC_TEST_TMP_DIR="${HC_TEST_TMP_DIR}"
  )

  # Only DrawObservations is covered. Overlay::Render needs a display and is
  # exercised by Task 14's manual run instead.
  hc_add_test(test_overlay test_overlay.cpp LIBS hc_app)
endif()
