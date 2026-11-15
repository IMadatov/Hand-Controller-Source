# hc_ffi — the C ABI over hc::Controller, built as a SHARED library.
#
# One artifact, several hosts: Dart FFI for the Flutter GUI first, and .NET
# P/Invoke or a mobile build after it, with no second implementation of the
# pipeline. Shared rather than static because that is what every FFI host can
# load at runtime.
#
# It sits at the TOP of the stack, beside the executable rather than below it:
# it links the platform layer (camera, uinput) and the ONNX backend, because
# choosing those is a frontend decision and this is a frontend.
add_library(hc_ffi SHARED src/hand_controller_ffi.cpp)

# The output name a host will dlopen/DynamicLibrary.open, deliberately not
# "libhc_ffi": the GUI loads "libhand_controller.so", which says what it is.
set_target_properties(hc_ffi PROPERTIES
  OUTPUT_NAME hand_controller
  # The whole ABI is the extern "C" surface in the public header; nothing else
  # is anyone's business, and hiding the rest keeps a C++ symbol from a host's
  # own libstdc++ colliding with ours.
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(hc_ffi PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hc_ffi PRIVATE
  hc_control
  hc_platform
  hc_vision_ort
)

# The same injected, absolute, fetch-time model paths the executable and the
# tracker tests get. A shared library cannot look beside its own binary for
# them: a host loads it from wherever it pleases.
target_compile_definitions(hc_ffi PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
  # Selects dllexport over dllimport in the public header. Only matters on
  # Windows; harmless elsewhere.
  HC_BUILDING_SHARED
)

# Hide every symbol that came from a STATIC dependency.
#
# Without this the library exports 147 symbols from OpenCV and ONNX Runtime
# while hiding its own API, and a host that carries its own copy of either can
# bind to ours -- which fails far from the cause. Measured with
# `nm -D --defined-only`, not assumed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(hc_ffi PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# PRIVATE above, so nothing linking hc_ffi inherits OpenCV or ONNX Runtime --
# a host sees the C header and nothing else, which is the point of the ABI.
hc_assert_does_not_link(hc_ffi FORBIDDEN hc_cli hand_controller)
