# The app layer: everything that is allowed to know about OpenCV. Only built
# when HC_BUILD_APP is ON (the parent guards this add_subdirectory).
#
# hc_app is a library rather than going straight into an executable so the
# test suite can link the same code the application runs. main.cpp is kept
# OUT of it: a library carrying a main() cannot be linked into a test binary
# that has its own.
add_library(hc_app STATIC
  src/opencv_frame_source.cpp
  src/overlay.cpp
  src/cli.cpp
)

# PUBLIC: test and app targets that link hc_app include its headers by plain
# name (e.g. "opencv_frame_source.hpp").
target_include_directories(hc_app PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# hc_vision_ort is PUBLIC because hc_app's own headers expose lower-layer
# types in their signatures (IFrameSource, Frame, ImageView, HandObservation),
# and linking it transitively carries hc_vision and hc_base. hc::opencv
# likewise, since cv::VideoCapture and cv::Mat appear as members of
# OpenCvFrameSource.
#
# This is the one layer in the project allowed to link OpenCV; every target
# below it is checked by hc_assert_no_forbidden_deps and must stay free of it.
target_link_libraries(hc_app PUBLIC hc_vision_ort hc::opencv)

# The executable. Only main.cpp: everything else it needs is in hc_app,
# which is the same code the tests link.
add_executable(hand_controller src/main.cpp)
target_link_libraries(hand_controller PRIVATE hc_app)

# The model paths are injected the same way the tracker tests get them --
# absolute, fetch-time paths from cmake/FetchModels.cmake -- rather than
# being hardcoded in the source or looked up relative to the binary.
target_compile_definitions(hand_controller PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
)
