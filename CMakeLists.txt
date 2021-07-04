cmake_minimum_required(VERSION 3.14)

project(refterm)

# ---- Shaders ----

set(shaders "")

foreach(type IN ITEMS Compute Pixel Vertex)
  string(SUBSTRING "${type}" 0 1 short)
  string(TOLOWER "${short}s" short)
  string(TOUPPER "${short}" uc_short)
  configure_file(refterm.hlsl "refterm_${short}.hlsl" COPYONLY)
  list(APPEND shaders "${PROJECT_BINARY_DIR}/refterm_${short}.hlsl")
  set_source_files_properties(
      "${PROJECT_BINARY_DIR}/refterm_${short}.hlsl" PROPERTIES
      VS_SHADER_FLAGS "/O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv"
      VS_SHADER_TYPE "${type}"
      VS_SHADER_MODEL 5.0
      VS_SHADER_ENTRYPOINT "${type}Main"
      VS_SHADER_OUTPUT_HEADER_FILE "refterm_${short}.h"
      VS_SHADER_VARIABLE_NAME "Refterm${uc_short}ShaderBytes"
  )
endforeach()

# ---- Declare refterm ----

add_executable(
    refterm WIN32
    refterm_example_dwrite.cpp
    refterm.c
    ${shaders}
)

target_compile_definitions(
    refterm PRIVATE
    COBJMACROS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _NO_CRT_STDIO_INLINE
)

target_compile_features(refterm PRIVATE c_std_90 cxx_std_11)

target_include_directories(
    refterm PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_BINARY_DIR}"
)

# ---- Declare splat ----

add_executable(splat splat.cpp)

target_compile_features(splat PRIVATE cxx_std_11)

target_compile_definitions(splat PRIVATE _CRT_SECURE_NO_WARNINGS)

target_include_directories(splat PRIVATE "${PROJECT_SOURCE_DIR}")

# ---- Install rules ----

install(TARGETS refterm RUNTIME COMPONENT refterm_Runtime)
install(TARGETS splat RUNTIME COMPONENT splat_Runtime)
