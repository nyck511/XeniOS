add_library(xenia-ui-metal STATIC
  metal_api.cc
  metal_api.h
  metal_gpu_completion_timeline.cc
  metal_gpu_completion_timeline.h
  metal_immediate_drawer.h
  metal_immediate_drawer.mm
  metal_presenter.h
  metal_presenter.mm
  metal_provider.cc
  metal_provider.h
)
target_include_directories(xenia-ui-metal PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/metal-cpp)
target_link_libraries(xenia-ui-metal PUBLIC
  xenia-base
  xenia-ui
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalKit"
  "-framework QuartzCore"
)
if(XE_PLATFORM_IOS)
  target_link_libraries(xenia-ui-metal PUBLIC "-framework UIKit")
else()
  target_link_libraries(xenia-ui-metal PUBLIC
    "-framework AppKit"
    "-framework MetalFX")
endif()
xe_shader_rules_metal(xenia-ui-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
xe_shader_rules_slang(xenia-ui-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                      TARGET msl)
xe_target_defaults(xenia-ui-metal)
