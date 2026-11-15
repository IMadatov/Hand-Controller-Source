# The application: argument parsing plus main(). Everything platform-specific
# lives in ../platform, and everything portable below that in ../base,
# ../vision.

# hc_cli — the command-line parser.
#
# A library rather than part of the executable so the tests can link the same
# code the application runs; main.cpp is kept OUT of it, because a library
# carrying a main() cannot be linked into a test binary that has its own.
#
# It links nothing. ParseCli touches only <string> and friends, which is why
# its test runs in the core-only configure with no OpenCV present -- see the
# unguarded hc_add_test(test_cli ...) in ../tests/CMakeLists.txt.
add_library(hc_cli STATIC src/cli.cpp)

target_include_directories(hc_cli PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Portable, and asserted to stay that way: parsing "--camera 0" has no
# business knowing which operating system it is on.
hc_assert_no_forbidden_deps(hc_cli)
hc_assert_does_not_link(hc_cli FORBIDDEN
  hc_base hc_vision hc_vision_ort hc_control hc_platform hand_controller
)

# The executable needs the platform layer, so it only exists when that layer
# was built. hc_cli above is unguarded on purpose: it is portable, and its
# tests should run in the fast core-only configure.
if(NOT HC_BUILD_APP)
  return()
endif()

# Only main.cpp: this is the one place in the project that knows about every
# layer at once, because wiring them together is precisely what it is for.
#
# hc_vision_ort appears here rather than in hc_platform because choosing the
# inference backend is an application decision. Swapping it for a different
# IHandTracker implementation is a change to this line.
add_executable(hand_controller src/main.cpp)
target_link_libraries(hand_controller PRIVATE
  hc_cli
  hc_platform
  hc_vision_ort
)

# The model paths are injected the same way the tracker tests get them --
# absolute, fetch-time paths from cmake/FetchModels.cmake -- rather than
# being hardcoded in the source or looked up relative to the binary.
target_compile_definitions(hand_controller PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
)
