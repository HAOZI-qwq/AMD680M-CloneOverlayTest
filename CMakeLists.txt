cmake_minimum_required(VERSION 3.20)
project(AMDCloneOverlayProbe LANGUAGES CXX)

add_executable(AMDCloneOverlayProbe WIN32 src/main.cpp)
target_compile_features(AMDCloneOverlayProbe PRIVATE cxx_std_17)
target_compile_definitions(AMDCloneOverlayProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(AMDCloneOverlayProbe PRIVATE d3d11 dxgi dcomp user32 gdi32 ole32)

if(MSVC)
  target_compile_options(AMDCloneOverlayProbe PRIVATE /W4 /permissive- /EHsc)
endif()
