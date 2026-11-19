# FFmpeg import libraries generated from the .def files of the LGPL shared build.
set(FFMPEG_DIR ${TP}/ffmpeg)
foreach(lib avcodec-63 avutil-61)
  string(REGEX REPLACE "-.*" "" short ${lib})
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${short}.lib
    COMMAND lib /nologo /machine:x64 /def:${FFMPEG_DIR}/def/${lib}.def /name:${lib}.dll /out:${CMAKE_BINARY_DIR}/${short}.lib
    DEPENDS ${FFMPEG_DIR}/def/${lib}.def)
  list(APPEND FFMPEG_IMPLIBS ${CMAKE_BINARY_DIR}/${short}.lib)
endforeach()
add_custom_target(ffmpeg_implibs DEPENDS ${FFMPEG_IMPLIBS})

add_library(wasd_engine STATIC
  src/platform/process.cpp src/platform/socket.cpp src/platform/adb.cpp src/platform/paths.cpp
  src/video/decoder.cpp src/session/session.cpp)
add_dependencies(wasd_engine ffmpeg_implibs)
target_include_directories(wasd_engine PUBLIC src ${FFMPEG_DIR}/include)
target_link_libraries(wasd_engine PUBLIC wasd_core ${FFMPEG_IMPLIBS} ws2_32 d3d11 dxgi bcrypt)
# FFmpeg headers trigger level-4 warnings we do not control.
target_compile_options(wasd_engine PRIVATE /wd4244 /wd4100)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/render/renderer.cpp)
  include(cmake/gui.cmake)
endif()
