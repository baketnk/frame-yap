cmake_minimum_required(VERSION 3.20)
project(frame_dictation VERSION 0.1.0 LANGUAGES CXX)

add_executable(frame-dictation src/main.cpp)
target_compile_features(frame-dictation PRIVATE cxx_std_20)
set_target_properties(frame-dictation PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(frame-dictation PRIVATE FRAME_DICTATION_VERSION="${PROJECT_VERSION}")

include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME frame_dictation.cli
    COMMAND "${CMAKE_COMMAND}"
      "-DAPP=$<TARGET_FILE:frame-dictation>"
      "-DEXPECTED_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake")
  add_test(NAME frame_dictation.install_preflight
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/install-preflight.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-preflight.sh")
endif()
