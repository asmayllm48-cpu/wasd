cmake_minimum_required(VERSION 3.24)
project(wasd LANGUAGES C CXX VERSION 0.1.0)

if(NOT MSVC)
  message(FATAL_ERROR "WASD targets Windows with MSVC (Visual Studio 2022).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
add_compile_options(/W4 /permissive- /utf-8 /MP)

set(TP ${CMAKE_SOURCE_DIR}/third_party)

# --- core (pure logic, unit tested) ---
add_library(wasd_core STATIC
  src/core/protocol.cpp src/core/coords.cpp src/core/stats.cpp src/core/log.cpp
  src/core/profile.cpp src/core/profile_io.cpp src/core/input_engine.cpp src/core/automation.cpp
  src/core/imgmatch.cpp src/core/studio_graph.cpp src/core/studio_engine.cpp
  src/core/studio_converter.cpp src/core/detector.cpp src/core/detect_tracker.cpp
  src/core/target_manager.cpp)
target_include_directories(wasd_core PUBLIC src ${TP}/nlohmann/.. ${TP})

# ONNX Runtime: object detection for the studio "Find Object" node. The static
# core lib carries the include path + import lib so both app and tests link.
set(ORT_DIR ${TP}/onnxruntime)
if(EXISTS ${ORT_DIR}/lib/onnxruntime.lib)
  target_include_directories(wasd_core PUBLIC ${ORT_DIR}/include)
  target_link_libraries(wasd_core PUBLIC ${ORT_DIR}/lib/onnxruntime.lib)
  target_compile_definitions(wasd_core PUBLIC WASD_WITH_ORT=1)
endif()

# --- tests ---
enable_testing()
add_executable(wasd_tests tests/test_main.cpp tests/test_protocol.cpp tests/test_coords.cpp
  tests/test_input_engine.cpp tests/test_profile.cpp tests/test_stats_queue.cpp tests/test_editor.cpp tests/test_automation.cpp
  tests/test_imgmatch.cpp tests/test_text_shaping.cpp tests/test_studio_graph.cpp
  tests/test_studio_engine.cpp tests/test_studio_converter.cpp tests/test_detector.cpp
  tests/test_detect_tracker.cpp tests/test_vision_pipeline.cpp src/app/text_shaping.cpp)
target_include_directories(wasd_tests PRIVATE ${TP})
target_link_libraries(wasd_tests PRIVATE wasd_core usp10 gdi32)
add_test(NAME wasd_tests COMMAND wasd_tests)

include(cmake/app.cmake OPTIONAL)
