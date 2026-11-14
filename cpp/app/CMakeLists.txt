# The app layer: everything that is allowed to know about OpenCV. Only built
# when HC_BUILD_APP is ON (the parent guards this add_subdirectory).
#
# hc_app is a library rather than going straight into an executable so the
# test suite can link the same code the application runs. The main() shell
# arrives in Task 14.
add_library(hc_app STATIC
  src/opencv_frame_source.cpp
  src/overlay.cpp
)

# PUBLIC: test and app targets that link hc_app include its headers by plain
# name (e.g. "opencv_frame_source.hpp").
target_include_directories(hc_app PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# hc_core is PUBLIC because hc_app's own headers expose core types in their
# signatures (IFrameSource, Frame, ImageView). hc::opencv likewise, since
# cv::VideoCapture and cv::Mat appear as members of OpenCvFrameSource.
#
# This is the one target in the project allowed to do this; hc_core itself is
# checked by hc_assert_no_forbidden_deps and must stay OpenCV-free.
target_link_libraries(hc_app PUBLIC hc_core hc::opencv)
