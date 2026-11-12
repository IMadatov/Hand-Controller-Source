# Fetches the official prebuilt ONNX Runtime release archive and exposes it
# as the imported target onnxruntime::onnxruntime.
#
# Deliberately NOT built from source: the source build pulls in Python and
# protobuf, which violates the "clean git clone + cmake" constraint for this
# project. We fetch the official release .tgz/.zip instead.
#
# Pinned version: 1.18.1 (see cpp/docs/models.md for the full record).

include(FetchContent)

set(HC_ONNXRUNTIME_VERSION "1.18.1")

if(WIN32)
  set(_hc_ort_archive "onnxruntime-win-x64-${HC_ONNXRUNTIME_VERSION}.zip")
  set(_hc_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${HC_ONNXRUNTIME_VERSION}/${_hc_ort_archive}")
  # SHA256 of onnxruntime-win-x64-1.18.1.zip (60,979,938 bytes), computed by
  # streaming the official GitHub release asset twice, in two independent
  # downloads that agreed, and confirming the result is a valid zip containing
  # lib/onnxruntime.dll and lib/onnxruntime.lib. See cpp/docs/models.md.
  set(_hc_ort_sha256 "53fb7226fe3cf16001afd1eae79e35f891a20e80bd686185d62ea878e6f9b1a6")
else()
  set(_hc_ort_archive "onnxruntime-linux-x64-${HC_ONNXRUNTIME_VERSION}.tgz")
  set(_hc_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${HC_ONNXRUNTIME_VERSION}/${_hc_ort_archive}")
  # SHA256 of onnxruntime-linux-x64-1.18.1.tgz, computed by hand from the
  # official GitHub release asset (see cpp/docs/models.md for how/when).
  set(_hc_ort_sha256 "a0994512ec1e1debc00c18bfc7a5f16249f6ebd6a6128ff2034464cc380ea211")
endif()

FetchContent_Declare(
  onnxruntime_release
  URL ${_hc_ort_url}
  URL_HASH SHA256=${_hc_ort_sha256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(onnxruntime_release)

if(WIN32)
  set(_hc_ort_lib "${onnxruntime_release_SOURCE_DIR}/lib/onnxruntime.lib")
  set(_hc_ort_dll "${onnxruntime_release_SOURCE_DIR}/lib/onnxruntime.dll")
else()
  set(_hc_ort_lib "${onnxruntime_release_SOURCE_DIR}/lib/libonnxruntime.so")
endif()

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_release_SOURCE_DIR}/include"
)

if(WIN32)
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_hc_ort_dll}"
    IMPORTED_IMPLIB "${_hc_ort_lib}"
  )
else()
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_hc_ort_lib}"
  )
endif()

set(HC_ONNXRUNTIME_ROOT "${onnxruntime_release_SOURCE_DIR}" CACHE PATH "ONNX Runtime release root")
