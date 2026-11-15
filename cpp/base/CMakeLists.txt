# hc_base — the shared primitives. The bottom of the dependency order: it
# depends on nothing in this project and on nothing heavyweight outside it.
#
# What belongs here is anything every layer above needs and no layer above
# owns: the coordinate and image types, the timing instrumentation, and the
# frame-source interface with its two portable implementations.
#
# Deliberately NOT linked to ONNX Runtime. hc_base is the layer a consumer
# can take without a 300 MB inference runtime -- a frame source and a Rect
# have no business requiring one. Only hc_vision_ort links ORT.
add_library(hc_base STATIC
  src/geometry.cpp
  src/metrics.cpp
  src/synthetic_frame_source.cpp
  src/latest_frame_source.cpp
)

target_include_directories(hc_base PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# LatestFrameSource runs its inner source on a std::thread, so hc_base needs
# the platform's threading library. Threads::Threads is a CMake-provided
# imported target, not a forbidden dependency.
find_package(Threads REQUIRED)
target_link_libraries(hc_base PUBLIC Threads::Threads)

hc_assert_no_forbidden_deps(hc_base)

# The bottom layer knows nothing about the layers above it. Stated as a build
# failure rather than as a comment, because a comment cannot fail.
hc_assert_does_not_link(hc_base FORBIDDEN
  hc_vision hc_vision_ort hc_control hc_platform hc_app
  onnxruntime::onnxruntime
)
