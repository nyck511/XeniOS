cmake_minimum_required(VERSION 3.20)

project(xenia-host-shader-compiler LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
  add_compile_options(/W0)
else()
  add_compile_options(-w)
endif()

get_filename_component(
  XENIA_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(XENIA_THIRD_PARTY_DIR "${XENIA_REPOSITORY_ROOT}/third_party")

include("${XENIA_REPOSITORY_ROOT}/cmake/HostShaderCompiler.cmake")

xe_add_host_spirv_tools(
  THIRD_PARTY_DIR "${XENIA_THIRD_PARTY_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/SPIRV-Tools")
xe_add_host_glslang(
  THIRD_PARTY_DIR "${XENIA_THIRD_PARTY_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/glslang-build-info")
xe_add_host_shader_compiler(
  THIRD_PARTY_DIR "${XENIA_THIRD_PARTY_DIR}"
  SOURCE "${XENIA_REPOSITORY_ROOT}/tools/build/shader_cc.cc"
  OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host_tools"
  METAL_MIN_OS "${CMAKE_OSX_DEPLOYMENT_TARGET}")
