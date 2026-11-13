# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.28.3)

# Reject any attempt to use a toolchain file. We must not use one because
# we could be downloading it here. If the CMAKE_TOOLCHAIN_FILE environment
# variable is set, the cache variable will have been initialized from it.
unset(CMAKE_TOOLCHAIN_FILE CACHE)
unset(ENV{CMAKE_TOOLCHAIN_FILE})

# We name the project and the target for the ExternalProject_Add() call
# to something that will highlight to the user what we are working on if
# something goes wrong and an error message is produced.

project(hc_palm_model-populate NONE)



include(ExternalProject)
ExternalProject_Add(hc_palm_model-populate
                     "UPDATE_DISCONNECTED" "False" "EXTERNALPROJECT_INTERNAL_ARGUMENT_SEPARATOR" "URL" "https://github.com/opencv/opencv_zoo/raw/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/palm_detection_mediapipe/palm_detection_mediapipe_2023feb.onnx" "URL_HASH" "SHA256=78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c" DOWNLOAD_NO_EXTRACT YES
                    SOURCE_DIR          "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-src"
                    BINARY_DIR          "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
                    USES_TERMINAL_PATCH     YES
)


ExternalProject_Get_Property(hc_palm_model-populate DOWNLOADED_FILE)
get_filename_component(dlFileName "${DOWNLOADED_FILE}" NAME)

ExternalProject_Add_Step(hc_palm_model-populate copyfile
  COMMAND    "/usr/bin/cmake" -E copy_if_different
             "<DOWNLOADED_FILE>" "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-src"
  DEPENDEES  patch
  DEPENDERS  configure
  BYPRODUCTS "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_palm_model-src/${dlFileName}"
  COMMENT    "Copying file to SOURCE_DIR"
)

