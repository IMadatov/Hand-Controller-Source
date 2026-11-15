# hc_platform — the per-system layer. The only layer in the project allowed
# to link OpenCV, and later the only one allowed to link X11/XTest or to call
# SendInput.
#
# Its single responsibility is platform effects: get frames in from a real
# device, put pixels out on a real screen, and (from the phase 2 injection
# tasks) put events out to a real OS. It holds no logic of its own. Deciding
# what a gesture means and what command it maps to happens below, in portable
# code, so that there is exactly one copy of that decision rather than one
# per operating system.
#
# A reader looking for "where does Linux differ from Windows" should find the
# answer entirely inside this directory.
add_library(hc_platform STATIC
  src/opencv_frame_source.cpp
  src/overlay.cpp
)

# PUBLIC: targets linking hc_platform include its headers by plain name
# (e.g. "overlay.hpp").
target_include_directories(hc_platform PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# hc_vision, not hc_vision_ort: the overlay draws HandObservations and the
# frame source produces Frames, and neither needs an inference runtime. The
# backend is the executable's concern, not the platform layer's.
#
# hc::opencv is PUBLIC because cv::VideoCapture and cv::Mat appear as members
# in OpenCvFrameSource's and Overlay's headers.
target_link_libraries(hc_platform PUBLIC hc_vision hc::opencv)

# No hc_assert_no_forbidden_deps here: OpenCV is this layer's whole point.
# The direction check still applies -- the platform layer is the top of the
# library stack and must not reach into the executable or the CLI parser.
hc_assert_does_not_link(hc_platform FORBIDDEN hc_cli hand_controller)
