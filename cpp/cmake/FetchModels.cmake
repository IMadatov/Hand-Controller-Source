# Fetches the pre-converted ONNX palm-detection and hand-landmark models.
#
# Source: OpenCV Zoo (https://github.com/opencv/opencv_zoo), Apache-2.0
# licensed, converted from the official MediaPipe TFLite models via tf2onnx
# + onnx-simplifier. Pinned to commit 48b10fa505aeb51fbd127fc60a5c660040e823b7
# so the URL keeps resolving the exact same bytes even after opencv_zoo's
# main branch moves on. The files are stored in that repo via Git LFS;
# github.com/.../raw/<commit>/... resolves the LFS object directly (verified
# by hand to match the LFS pointer's recorded oid; see cpp/docs/models.md).
#
# Models are NOT committed to git: fetched here by URL with a pinned SHA256.

include(FetchContent)

set(_hc_opencv_zoo_commit "48b10fa505aeb51fbd127fc60a5c660040e823b7")
set(_hc_opencv_zoo_base "https://github.com/opencv/opencv_zoo/raw/${_hc_opencv_zoo_commit}")

FetchContent_Declare(
  hc_palm_model
  URL "${_hc_opencv_zoo_base}/models/palm_detection_mediapipe/palm_detection_mediapipe_2023feb.onnx"
  URL_HASH SHA256=78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Populate(hc_palm_model)

FetchContent_Declare(
  hc_landmark_model
  URL "${_hc_opencv_zoo_base}/models/handpose_estimation_mediapipe/handpose_estimation_mediapipe_2023feb.onnx"
  URL_HASH SHA256=db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Populate(hc_landmark_model)

set(HC_PALM_MODEL "${hc_palm_model_SOURCE_DIR}/palm_detection_mediapipe_2023feb.onnx" CACHE FILEPATH "Absolute path to the palm-detection ONNX model")
set(HC_LANDMARK_MODEL "${hc_landmark_model_SOURCE_DIR}/handpose_estimation_mediapipe_2023feb.onnx" CACHE FILEPATH "Absolute path to the hand-landmark ONNX model")
