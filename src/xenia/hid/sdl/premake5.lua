project_root = "../../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-hid-sdl")
  uuid("44f5b9a1-00f8-4825-acf1-5c93f26eba9b")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base",
    "xenia-hid",
    "xenia-ui",
    "SDL2",
  })
  local_platform_files()
  sdl2_include()
  filter("system:ios")
    local ios_embedded_dir =
        path.getabsolute(path.join(project_root, "build/generated/xenia-hid-sdl"))
    files({
      path.join(ios_embedded_dir, "embedded_bundle_gamecontrollerdb.cc"),
      path.join(ios_embedded_dir, "embedded_bundle_gamecontrollerdb.h"),
    })
    includedirs({
      ios_embedded_dir,
    })
  filter({})
