cmake_minimum_required(VERSION 3.20)
project(AMDCloneOverlayProbe LANGUAGES CXX)

# V3 is intentionally a console probe so the user can see the KMT/MPO
# diagnostics and explicitly choose whether to attempt direct MPO present.
add_executable(AMDCloneOverlayProbe src/v3.cpp)
target_compile_features(AMDCloneOverlayProbe PRIVATE cxx_std_17)
target_compile_definitions(AMDCloneOverlayProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(AMDCloneOverlayProbe PRIVATE d3d11 dxgi user32 gdi32)

if(MSVC)
  # d3dkmthk.h is a WDK-facing header. Force the desktop Win32/NT type
  # declarations in first so the normal Windows SDK build can consume it.
  target_compile_options(AMDCloneOverlayProbe PRIVATE /W4 /permissive- /EHsc /FI"${CMAKE_CURRENT_SOURCE_DIR}/src/wdk_compat.h")
endif()
