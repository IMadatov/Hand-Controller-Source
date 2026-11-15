# hc_control — the logic layer: what a pose means, and which command it maps
# to. Gesture recognition (Tasks 2-3), the command vocabulary (Task 4),
# bindings (Task 5) and the mapper (Task 6) all live here.
#
# This is where the application's decisions are made, ONCE, in portable code.
# None of it may move up into hc_platform: a per-system layer holding gesture
# logic would hold one copy per operating system, and they would diverge
# within a release.
#
# Pure by construction and by assertion: no OS, no OpenCV, no inference
# runtime, and no clock it does not receive as an argument. Every timestamp
# arrives as a parameter, which is what lets the whole layer be tested from
# synthetic landmark sequences with no camera and no display.
add_library(hc_control STATIC
  src/gesture_recognizer.cpp
)

target_include_directories(hc_control PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# hc_vision for HandObservation and HandFeatures, which are the layer's whole
# input. PUBLIC because they appear in hc_control's own signatures.
target_link_libraries(hc_control PUBLIC hc_vision)

hc_assert_no_forbidden_deps(hc_control)

# Not hc_vision_ort, and the assertion is the point rather than a formality:
# gesture recognition consumes HandObservation, which is plain data, so it has
# no business dragging in an inference runtime. Tying this layer to ORT would
# also tie it to the one backend, and a WebAssembly build delegating inference
# to JavaScript still needs the gesture logic.
hc_assert_does_not_link(hc_control FORBIDDEN
  hc_vision_ort hc_platform hc_cli hand_controller
  onnxruntime::onnxruntime
)
