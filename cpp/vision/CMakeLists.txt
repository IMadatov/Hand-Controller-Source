# The vision layer: finding and tracking hands. Split into two targets on the
# one seam that matters -- whether a file needs an inference runtime.
#
#   hc_vision      the algorithms and the IHandTracker interface. No runtime.
#   hc_vision_ort  the ONNX Runtime implementation of that interface.
#
# This is the standalone hand-detection module. Nothing in here knows what a
# gesture is, what a command is, where frames come from, or how anything is
# drawn: it turns pixels into normalized landmarks and stops. Everything above
# it -- gesture recognition, mapping, injection, any UI -- depends on it, and
# it depends on none of them.

# ---------------------------------------------------------------------------
# hc_vision — the portable algorithms.
#
# Anchor generation, palm-detection decoding with weighted NMS, ROI
# derivation, the rotated-crop resampler, track association, and the derived
# per-hand features. Plus hand_tracker.hpp, which declares IHandTracker.
#
# ORT-free on purpose. These are the parts that can be compiled anywhere a
# C++17 compiler exists -- including to WebAssembly, where the inference
# itself would be delegated to a JavaScript runtime rather than linked. That
# port needs exactly this seam, so the seam is drawn now while it is free.
add_library(hc_vision STATIC
  src/anchors.cpp
  src/palm_decode.cpp
  src/roi.cpp
  src/warp.cpp
  src/association.cpp
  src/hand_features.cpp
)

target_include_directories(hc_vision PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# PUBLIC: hc_vision's headers expose hc_base types in their signatures
# (Point2, Rect, ImageView), so anything linking hc_vision needs them too.
target_link_libraries(hc_vision PUBLIC hc_base)

hc_assert_no_forbidden_deps(hc_vision)

# Direction of dependency, enforced. hc_vision may use hc_base and nothing
# else of ours; in particular it must not reach up into gesture logic, and it
# must not acquire an inference runtime -- that is hc_vision_ort's whole job.
hc_assert_does_not_link(hc_vision FORBIDDEN
  hc_vision_ort hc_control hc_platform hc_app
  onnxruntime::onnxruntime
)

# ---------------------------------------------------------------------------
# hc_vision_ort — the ONNX Runtime backend.
#
# One translation unit, and the only file in the project that includes
# <onnxruntime_cxx_api.h>. A second backend (ORT Web via WASM, NNAPI,
# Core ML) becomes a sibling target implementing the same IHandTracker,
# not an edit to the algorithms above.
add_library(hc_vision_ort STATIC
  src/hand_tracker_onnx.cpp
)

target_link_libraries(hc_vision_ort PUBLIC hc_vision onnxruntime::onnxruntime)

# ORT is the one heavyweight dependency allowed below the platform layer: it
# is prebuilt, hash-pinned, and runs on Android, unlike OpenCV and X11 which
# hc_assert_no_forbidden_deps still rejects here.
hc_assert_no_forbidden_deps(hc_vision_ort)

hc_assert_does_not_link(hc_vision_ort FORBIDDEN
  hc_control hc_platform hc_app
)
