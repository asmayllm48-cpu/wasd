add_library(imgui STATIC
  ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp ${TP}/imgui/imgui_widgets.cpp
  ${TP}/imgui/backends/imgui_impl_win32.cpp ${TP}/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends)
target_compile_options(imgui PRIVATE /W0)

add_library(wasd_render STATIC src/render/renderer.cpp)
target_link_libraries(wasd_render PUBLIC wasd_engine d3dcompiler)
target_compile_options(wasd_render PRIVATE /wd4244 /wd4100)

# Headless measurement tool: connects, decodes, writes CSV (no window).
add_executable(wasd-bench src/bench/bench_main.cpp)
target_link_libraries(wasd-bench PRIVATE wasd_engine)
target_compile_options(wasd-bench PRIVATE /wd4244)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/app/main.cpp)
  file(GLOB APP_SOURCES ${CMAKE_SOURCE_DIR}/src/app/*.cpp ${CMAKE_SOURCE_DIR}/src/app/*.rc)
  add_executable(wasd WIN32 ${APP_SOURCES})
  target_link_libraries(wasd PRIVATE wasd_render imgui comdlg32 psapi dwmapi usp10 gdi32 winmm)
  target_compile_options(wasd PRIVATE /wd4244 /wd4100)
endif()

# Copy runtime (adb, scrcpy-server, FFmpeg DLLs) next to the executables when present.
set(RUNTIME_SRC ${CMAKE_SOURCE_DIR}/runtime)
if(EXISTS ${RUNTIME_SRC}/adb.exe)
  add_custom_target(copy_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUNTIME_SRC}/adb.exe ${RUNTIME_SRC}/AdbWinApi.dll ${RUNTIME_SRC}/AdbWinUsbApi.dll ${RUNTIME_SRC}/scrcpy-server ${CMAKE_BINARY_DIR}/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUNTIME_SRC}/avcodec-63.dll ${RUNTIME_SRC}/avutil-61.dll ${CMAKE_BINARY_DIR})
endif()

# ONNX Runtime DLLs beside the exe (+ test exe) when the vendored package exists.
if(EXISTS ${ORT_DIR}/lib/onnxruntime.dll)
  add_custom_target(copy_ort ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DIR}/lib/onnxruntime.dll ${ORT_DIR}/lib/onnxruntime_providers_shared.dll ${CMAKE_BINARY_DIR})
endif()
file(GLOB MODEL_FILES ${RUNTIME_SRC}/models/*.onnx)
if(MODEL_FILES)
  add_custom_target(copy_model ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/runtime/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MODEL_FILES} ${CMAKE_BINARY_DIR}/runtime/models)
endif()
