# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-src"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-build"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix/tmp"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix/src/onnxruntime_release-populate-stamp"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix/src"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix/src/onnxruntime_release-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix/src/onnxruntime_release-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/onnxruntime_release-subbuild/onnxruntime_release-populate-prefix/src/onnxruntime_release-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
