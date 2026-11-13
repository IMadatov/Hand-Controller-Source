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

project(hc_landmark_model-populate NONE)



include(ExternalProject)
ExternalProject_Add(hc_landmark_model-populate
                     "UPDATE_DISCONNECTED" "False" "EXTERNALPROJECT_INTERNAL_ARGUMENT_SEPARATOR" "URL" "https://github.com/opencv/opencv_zoo/raw/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/handpose_estimation_mediapipe/handpose_estimation_mediapipe_2023feb.onnx" "URL_HASH" "SHA256=db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c" DOWNLOAD_NO_EXTRACT YES
                    SOURCE_DIR          "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_landmark_model-src"
                    BINARY_DIR          "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_landmark_model-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
                    USES_TERMINAL_PATCH     YES
)


ExternalProject_Get_Property(hc_landmark_model-populate DOWNLOADED_FILE)
get_filename_component(dlFileName "${DOWNLOADED_FILE}" NAME)

ExternalProject_Add_Step(hc_landmark_model-populate copyfile
  COMMAND    "/usr/bin/cmake" -E copy_if_different
             "<DOWNLOADED_FILE>" "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_landmark_model-src"
  DEPENDEES  patch
  DEPENDERS  configure
  BYPRODUCTS "/home/user01/Documents/GitHub/Hand-Controller-Source/cpp/build-asan/_deps/hc_landmark_model-src/${dlFileName}"
  COMMENT    "Copying file to SOURCE_DIR"
)

