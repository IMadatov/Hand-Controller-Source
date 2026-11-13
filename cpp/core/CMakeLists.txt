add_library(hc_core STATIC
  src/geometry.cpp
  src/metrics.cpp
  src/warp.cpp
  src/anchors.cpp
  src/palm_decode.cpp
  src/roi.cpp
  src/hand_tracker_onnx.cpp
)

target_include_directories(hc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ONNX Runtime is the one heavyweight dependency hc_core is allowed: it is a
# prebuilt, pinned, Android-capable inference runtime, unlike OpenCV/X11 which
# hc_assert_no_forbidden_deps below still rejects. PUBLIC so test and app
# targets inherit the include path and the imported shared library.
target_link_libraries(hc_core PUBLIC onnxruntime::onnxruntime)

hc_assert_no_forbidden_deps(hc_core)
