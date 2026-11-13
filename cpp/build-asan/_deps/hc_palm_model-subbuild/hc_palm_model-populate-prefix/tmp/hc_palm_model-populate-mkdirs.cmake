# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-src"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-build"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix/tmp"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix/src/hc_palm_model-populate-stamp"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix/src"
  "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix/src/hc_palm_model-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix/src/hc_palm_model-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-subbuild/hc_palm_model-populate-prefix/src/hc_palm_model-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
