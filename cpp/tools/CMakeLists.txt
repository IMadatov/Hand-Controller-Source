add_executable(validate_models validate_models.cpp)
target_link_libraries(validate_models PRIVATE onnxruntime::onnxruntime)
target_compile_definitions(validate_models PRIVATE
  HC_PALM_MODEL_PATH="${HC_PALM_MODEL}"
  HC_LANDMARK_MODEL_PATH="${HC_LANDMARK_MODEL}"
  HC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures"
)

# xtest_probe — answers "does XTest actually move the cursor on this machine?"
#
# Committed rather than left as a scratch file because the answer is a
# property of the session, not of the code, and it has to be re-checked
# whenever that session changes. XTest reports itself present and returns
# success on Wayland while moving nothing, so the only trustworthy check
# reads the pointer position back -- which is what this does.
#
# Needs no X11 development package and adds no build dependency: it declares
# the few types it uses and resolves every symbol through dlopen, the same way
# the X11 sink is required to.
if(UNIX AND NOT APPLE)
  add_executable(xtest_probe xtest_probe.cpp)
  target_link_libraries(xtest_probe PRIVATE ${CMAKE_DL_LIBS})
endif()
