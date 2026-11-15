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
  src/command.cpp
  src/bindings.cpp
)

target_include_directories(hc_control PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The shipped default configuration is compiled in, expanded from the very
# file that is installed rather than retyped into a string literal. Two copies
# of a configuration format desynchronize on the first edit to either, and
# nothing would report it.
set(HC_DEFAULT_BINDINGS_FILE
  "${CMAKE_CURRENT_SOURCE_DIR}/../config/default_bindings.txt")
file(READ "${HC_DEFAULT_BINDINGS_FILE}" HC_DEFAULT_BINDINGS_TEXT)

# file(READ) creates no dependency of its own, so without this an edit to the
# .txt would not re-run CMake and the binary would keep the stale text.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${HC_DEFAULT_BINDINGS_FILE}")

configure_file(default_bindings.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/hc/default_bindings.hpp" @ONLY)

# PRIVATE: the generated header is an implementation detail of bindings.cpp.
# DefaultBindingsText() in the public header is how callers reach the text.
target_include_directories(hc_control PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

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
