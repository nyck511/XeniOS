project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-patcher")
  uuid("e1c75f76-9e7b-48f6-b17e-dbd20f7a1592")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base"
  })
  recursive_platform_files()
  filter("system:ios")
    local ios_embedded_dir =
        path.getabsolute(path.join(project_root, "build/generated/xenia-patcher"))
    files({
      path.join(ios_embedded_dir, "embedded_bundle_patches.cc"),
      path.join(ios_embedded_dir, "embedded_bundle_patches.h"),
    })
    includedirs({
      ios_embedded_dir,
    })
  filter({})
