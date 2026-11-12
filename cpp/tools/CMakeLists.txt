add_executable(validate_models validate_models.cpp)
target_link_libraries(validate_models PRIVATE onnxruntime::onnxruntime)
target_compile_definitions(validate_models PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
  HC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures"
)
