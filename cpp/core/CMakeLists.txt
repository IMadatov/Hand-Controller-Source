add_library(hc_core STATIC
  src/geometry.cpp
  src/metrics.cpp
  src/warp.cpp
  src/anchors.cpp
  src/palm_decode.cpp
  src/roi.cpp
)

target_include_directories(hc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

hc_assert_no_forbidden_deps(hc_core)
