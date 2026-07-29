# Focused dependency definitions for the native xenia-shader-cc build.
#
# This file is shared by the full third-party graph and the dedicated
# tools/build/shader_cc project. Keep it limited to dependencies that the host
# shader compiler links directly.

include(CMakeParseArguments)

function(xe_add_host_spirv_tools)
  cmake_parse_arguments(
    ARG
    ""
    "THIRD_PARTY_DIR;BINARY_DIR"
    ""
    ${ARGN})
  if(NOT ARG_THIRD_PARTY_DIR OR NOT ARG_BINARY_DIR)
    message(FATAL_ERROR
      "xe_add_host_spirv_tools requires THIRD_PARTY_DIR and BINARY_DIR")
  endif()

  set(SPIRV-Headers_SOURCE_DIR
      "${ARG_THIRD_PARTY_DIR}/SPIRV-Headers" CACHE INTERNAL "")
  set(SPIRV_SKIP_TESTS ON CACHE INTERNAL "")
  set(SPIRV_SKIP_EXECUTABLES ON CACHE INTERNAL "")
  set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE INTERNAL "")
  set(SPIRV_WERROR OFF CACHE INTERNAL "")
  set(SPIRV_BUILD_FUZZER OFF CACHE INTERNAL "")
  add_subdirectory(
    "${ARG_THIRD_PARTY_DIR}/SPIRV-Tools"
    "${ARG_BINARY_DIR}"
    EXCLUDE_FROM_ALL)

  # SPIRV-Tools unconditionally forces /W2 /WX on MSVC via its own
  # target_compile_options. Strip those flags so the enclosing project's
  # warning policy remains authoritative.
  if(MSVC)
    foreach(_spvt_target
        SPIRV-Tools-static SPIRV-Tools-shared SPIRV-Tools-opt
        SPIRV-Tools-link SPIRV-Tools-reduce SPIRV-Tools-diff)
      if(TARGET ${_spvt_target})
        get_target_property(_opts ${_spvt_target} COMPILE_OPTIONS)
        if(_opts)
          list(FILTER _opts EXCLUDE REGEX "^/[Ww]([0-9]|all|X)$")
          set_target_properties(
            ${_spvt_target} PROPERTIES COMPILE_OPTIONS "${_opts}")
        endif()
      endif()
    endforeach()
  endif()
endfunction()

function(xe_add_host_glslang)
  cmake_parse_arguments(
    ARG
    ""
    "THIRD_PARTY_DIR;BINARY_DIR"
    ""
    ${ARGN})
  if(NOT ARG_THIRD_PARTY_DIR OR NOT ARG_BINARY_DIR)
    message(FATAL_ERROR
      "xe_add_host_glslang requires THIRD_PARTY_DIR and BINARY_DIR")
  endif()
  if(NOT TARGET SPIRV-Tools-static OR NOT TARGET SPIRV-Tools-opt)
    message(FATAL_ERROR
      "xe_add_host_glslang requires SPIRV-Tools-static and SPIRV-Tools-opt")
  endif()

  set(_glslang_dir "${ARG_THIRD_PARTY_DIR}/glslang")
  include("${_glslang_dir}/parse_version.cmake")
  parse_version("${_glslang_dir}/CHANGES.md" GLSLANG)
  set(major ${GLSLANG_VERSION_MAJOR})
  set(minor ${GLSLANG_VERSION_MINOR})
  set(patch ${GLSLANG_VERSION_PATCH})
  set(flavor ${GLSLANG_VERSION_FLAVOR})
  configure_file(
    "${_glslang_dir}/build_info.h.tmpl"
    "${ARG_BINARY_DIR}/glslang/build_info.h"
    @ONLY)

  file(GLOB _glslang_mi
    "${_glslang_dir}/glslang/MachineIndependent/*.cpp"
    "${_glslang_dir}/glslang/MachineIndependent/preprocessor/*.cpp"
    "${_glslang_dir}/glslang/GenericCodeGen/*.cpp")
  file(GLOB _glslang_spirv_all "${_glslang_dir}/SPIRV/*.cpp")
  add_library(glslang STATIC
    ${_glslang_mi}
    ${_glslang_spirv_all}
    "${_glslang_dir}/glslang/ResourceLimits/ResourceLimits.cpp")
  if(WIN32)
    target_sources(
      glslang PRIVATE
      "${_glslang_dir}/glslang/OSDependent/Windows/ossource.cpp")
  else()
    target_sources(
      glslang PRIVATE
      "${_glslang_dir}/glslang/OSDependent/Unix/ossource.cpp")
  endif()
  target_include_directories(glslang PUBLIC
    "${_glslang_dir}"
    "${ARG_BINARY_DIR}")
  target_compile_features(glslang PUBLIC cxx_std_17)
  target_compile_definitions(glslang PRIVATE ENABLE_OPT=1)
  target_link_libraries(
    glslang PUBLIC SPIRV-Tools-opt SPIRV-Tools-static)
  if(MSVC)
    target_compile_options(glslang PRIVATE /EHsc)
  endif()
  if(NOT WIN32)
    find_package(Threads)
    if(Threads_FOUND)
      target_link_libraries(glslang PUBLIC Threads::Threads)
    endif()
  endif()
endfunction()

function(xe_add_host_shader_compiler)
  cmake_parse_arguments(
    ARG
    ""
    "THIRD_PARTY_DIR;SOURCE;OUTPUT_DIRECTORY;METAL_MIN_OS"
    ""
    ${ARGN})
  if(NOT ARG_THIRD_PARTY_DIR OR NOT ARG_SOURCE OR
      NOT ARG_OUTPUT_DIRECTORY)
    message(FATAL_ERROR
      "xe_add_host_shader_compiler requires THIRD_PARTY_DIR, SOURCE and "
      "OUTPUT_DIRECTORY")
  endif()
  if(NOT TARGET glslang OR NOT TARGET SPIRV-Tools-static)
    message(FATAL_ERROR
      "xe_add_host_shader_compiler requires glslang and SPIRV-Tools-static")
  endif()

  add_executable(xenia-shader-cc "${ARG_SOURCE}")
  target_link_libraries(
    xenia-shader-cc PRIVATE glslang SPIRV-Tools-static)
  target_include_directories(
    xenia-shader-cc PRIVATE
    "${ARG_THIRD_PARTY_DIR}/glslang/StandAlone")
  target_compile_features(xenia-shader-cc PRIVATE cxx_std_20)
  set_target_properties(
    xenia-shader-cc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}")
  if(APPLE)
    if(ARG_METAL_MIN_OS)
      set(_metal_min_os "${ARG_METAL_MIN_OS}")
    else()
      set(_metal_min_os "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    target_compile_definitions(xenia-shader-cc PRIVATE
      XE_SHADER_CC_METAL=1
      XE_METAL_MIN_OS="${_metal_min_os}")
  endif()
  if(MSVC)
    target_compile_options(xenia-shader-cc PRIVATE /EHsc)
  endif()
endfunction()
