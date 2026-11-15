# Fetches the OpenCV source release and exposes the subset this project uses
# as the interface target hc::opencv.
#
# Built FROM SOURCE, unlike ONNX Runtime: there is no official prebuilt OpenCV
# release for Linux, and the system package is deliberately absent (the spec
# records `pkg-config --modversion opencv4` failing as part of the baseline).
# Building it ourselves is also what lets us trim the module list, which is the
# difference between a few minutes and a very long build.
#
# The whole file is guarded by HC_BUILD_APP: OpenCV belongs to the app and
# overlay layers only. Tasks 2-11 configure with -DHC_BUILD_APP=OFF and must
# neither fetch nor build any of this, and hc_core must never link it
# (enforced separately by hc_assert_no_forbidden_deps).
#
# Pinned version: 4.14.0.

if(NOT HC_BUILD_APP)
  return()
endif()

include(FetchContent)

set(HC_OPENCV_VERSION "4.14.0")

# SHA256 of the GitHub source archive for tag 4.14.0 (96,287,941 bytes).
# Computed from the downloaded asset, which was then validated structurally:
# it extracts cleanly (9417 entries), carries all five modules named in
# HC_OPENCV_MODULES below, and its modules/core/include/opencv2/core/version.hpp
# declares exactly MAJOR 4 / MINOR 14 / REVISION 0 with an empty STATUS.
#
# NOTE: this pin rests on a single download plus that structural check, not on
# two independent downloads agreeing as HC_ONNXRUNTIME's hash does, and OpenCV
# publishes no signed checksum for generated archive tarballs to check against.
set(HC_OPENCV_SHA256 "ee8fb9b30eb60850431b4656447080e3737b56e45719c92b67f245950609f86e")

# The only modules the project uses: core/imgproc for types and drawing,
# imgcodecs because videoio's MJPEG paths depend on it, videoio for camera and
# file capture (Task 12), highgui for the debug window (Task 13). Everything
# else -- dnn, gapi, calib3d, features2d, objdetect, photo, stitching, ml,
# video -- stays unbuilt. Inference is ONNX Runtime's job, not OpenCV's.
set(HC_OPENCV_MODULES core imgproc imgcodecs videoio highgui)
string(REPLACE ";" "," _hc_opencv_build_list "${HC_OPENCV_MODULES}")

# OpenCV reads its configuration from cache variables, so these must all be set
# before FetchContent_MakeAvailable runs its add_subdirectory.
set(BUILD_LIST "${_hc_opencv_build_list}" CACHE STRING "" FORCE)

# Static, so that test and app binaries need no LD_LIBRARY_PATH to run under
# ctest. hc_core is declared STATIC explicitly, so forcing this is safe.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(OPENCV_SKIP_PYTHON_LOADER ON CACHE BOOL "" FORCE)

# Nothing in OpenCV's own test/sample/tool/binding surface is wanted; these are
# the bulk of a default OpenCV build.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_apps OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python2 OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python3 OFF CACHE BOOL "" FORCE)
set(BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(OPENCV_GENERATE_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(OPENCV_GENERATE_SETUPVARS OFF CACHE BOOL "" FORCE)

# Vendored libjpeg/libpng/zlib ship inside the tarball, so imgcodecs needs no
# system image libraries and no network access beyond the one archive.
set(BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(BUILD_JPEG ON CACHE BOOL "" FORCE)
set(BUILD_PNG ON CACHE BOOL "" FORCE)

# Formats and codecs the project never decodes. Dropping them cuts build time
# and, for OpenEXR and TIFF, removes sizeable attack surface we would be
# carrying for no reason.
set(WITH_TIFF OFF CACHE BOOL "" FORCE)
set(WITH_WEBP OFF CACHE BOOL "" FORCE)
set(WITH_OPENEXR OFF CACHE BOOL "" FORCE)
set(WITH_OPENJPEG OFF CACHE BOOL "" FORCE)
set(WITH_JASPER OFF CACHE BOOL "" FORCE)

# IPP and the protobuf/ADE dependencies would each trigger their own 3rdparty
# download at configure time, which breaks the "one pinned archive" property
# this file is built around. Protobuf and ADE serve dnn and gapi, neither of
# which is in BUILD_LIST at all.
set(WITH_IPP OFF CACHE BOOL "" FORCE)
set(WITH_PROTOBUF OFF CACHE BOOL "" FORCE)
set(BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(WITH_ADE OFF CACHE BOOL "" FORCE)

# Acceleration and numerics backends that are either absent here or irrelevant
# to a BGR capture-and-draw path.
set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
set(WITH_CUDA OFF CACHE BOOL "" FORCE)
set(WITH_EIGEN OFF CACHE BOOL "" FORCE)
set(WITH_TBB OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(WITH_QUIRC OFF CACHE BOOL "" FORCE)

# The two capture backends that matter on Linux. V4L is the camera path Task 12
# negotiates MJPEG over, and GTK backs the Task 13 debug window; both have their
# dev packages installed as a prerequisite of this task.
set(WITH_V4L ON CACHE BOOL "" FORCE)
set(WITH_LIBV4L ON CACHE BOOL "" FORCE)
set(WITH_GTK ON CACHE BOOL "" FORCE)
set(WITH_GTK_2_X OFF CACHE BOOL "" FORCE)

# Opportunistic: OpenCV enables FFmpeg only if the system dev libraries are
# present, and silently disables it otherwise. It is NOT installed here, so
# container formats like .mp4 will not open. Nothing in the test suite may
# depend on it -- the Task 12 tests generate .avi with the MJPG fourcc, which
# OpenCV's built-in MJPEG encoder handles with no external codec at all.
set(WITH_FFMPEG ON CACHE BOOL "" FORCE)
set(WITH_GSTREAMER OFF CACHE BOOL "" FORCE)

# Pin where cvconfig.h and opencv2/opencv_modules.hpp are generated, so the
# include list below does not have to guess at it.
set(OPENCV_CONFIG_FILE_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/opencv_config" CACHE PATH "" FORCE)

FetchContent_Declare(
  opencv_source
  URL "https://github.com/opencv/opencv/archive/refs/tags/${HC_OPENCV_VERSION}.tar.gz"
  URL_HASH SHA256=${HC_OPENCV_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(opencv_source)

# OpenCV consumed via add_subdirectory does not populate usable
# INTERFACE_INCLUDE_DIRECTORIES on its module targets the way an installed
# OpenCVConfig.cmake does, so the include path is assembled by hand: one
# include dir per module, plus the generated-header dir pinned above.
set(_hc_opencv_includes "${OPENCV_CONFIG_FILE_INCLUDE_DIR}")
foreach(_hc_module ${HC_OPENCV_MODULES})
  list(APPEND _hc_opencv_includes
    "${opencv_source_SOURCE_DIR}/modules/${_hc_module}/include")
endforeach()

set(_hc_opencv_libs "")
foreach(_hc_module ${HC_OPENCV_MODULES})
  list(APPEND _hc_opencv_libs "opencv_${_hc_module}")
endforeach()

# Single target for app-layer code to link. Named hc::opencv rather than
# re-exporting the opencv_* targets so that there is exactly one place where
# this dependency enters the project, and so hc_assert_no_forbidden_deps has an
# obvious thing to catch if it ever reaches hc_core.
add_library(hc_opencv INTERFACE)
target_include_directories(hc_opencv SYSTEM INTERFACE ${_hc_opencv_includes})
target_link_libraries(hc_opencv INTERFACE ${_hc_opencv_libs})
add_library(hc::opencv ALIAS hc_opencv)
