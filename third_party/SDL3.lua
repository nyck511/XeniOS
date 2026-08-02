-- SDL3 dependency bridge for the Premake build.
--
-- iPhoneOS uses the pinned SDL3 submodule through the deterministic
-- out-of-source CMake build prepared by xenia-build.py. Desktop Premake builds
-- use an SDL3 development package rather than combining SDL3 headers with the
-- obsolete SDL2 library.

local third_party_path = os.getcwd()
local repository_root = path.getabsolute(path.join(third_party_path, ".."))
local source_include_dir =
    path.getabsolute(path.join(third_party_path, "SDL3", "include"))

local function is_ios_target()
  return
      os.isfile(path.join(repository_root, ".ios_target")) or
      os.getenv("XE_TARGET_IOS") == "1" or
      (os.target and os.target() == "ios") or
      os.istarget("ios") or
      (_OPTIONS and _OPTIONS["os"] == "ios")
end

local include_dirs = {}
local library_dirs = {}
local libraries = {}
local extra_link_options = {}

if is_ios_target() then
  local build_dir = path.getabsolute(
      path.join(repository_root, "build", "ios-sdl3"))
  local archive = path.join(build_dir, "libSDL3.a")
  local revision_header =
      path.join(build_dir, "include-revision", "SDL3", "SDL_revision.h")

  if not os.isfile(archive) or not os.isfile(revision_header) then
    error(
        "Pinned iPhoneOS SDL3 outputs are missing. Generate through " ..
        "./xenia-build.py build --target_os=ios so SDL3-static is " ..
        "prepared before Premake.")
  end

  include_dirs = {
    path.join(build_dir, "include-revision"),
    source_include_dir,
  }
  library_dirs = {build_dir}
  libraries = {
    "SDL3",
    "m",
    "CoreMedia.framework",
    "CoreVideo.framework",
    "CoreAudio.framework",
    "AudioToolbox.framework",
    "AVFoundation.framework",
    "CoreBluetooth.framework",
    "CoreGraphics.framework",
    "CoreMotion.framework",
    "Foundation.framework",
    "GameController.framework",
    "Metal.framework",
    "OpenGLES.framework",
    "QuartzCore.framework",
    "UIKit.framework",
  }
  extra_link_options = {
    "-lpthread",
    "-Wl,-weak_framework,CoreHaptics",
  }
  print("SDL3: using pinned iPhoneOS static archive " .. archive)
elseif os.istarget("linux") then
  local pkg_config = os.getenv("PKG_CONFIG") or "pkg-config"
  local cflags = os.outputof(pkg_config .. " --cflags-only-I sdl3")
  local libdirs_output = os.outputof(pkg_config .. " --libs-only-L sdl3")
  local libraries_output = os.outputof(pkg_config .. " --libs-only-l sdl3")
  local link_options_output =
      os.outputof(pkg_config .. " --libs-only-other sdl3")
  if not cflags or not libraries_output or libraries_output == "" then
    error(
        "SDL3 development files were not found through pkg-config. " ..
        "Install an SDL3 package or use the repository CMake build.")
  end
  for include_dir in string.gmatch(cflags, "-I([^%s]+)") do
    table.insert(include_dirs, include_dir)
  end
  for library_dir in string.gmatch(libdirs_output or "", "-L([^%s]+)") do
    table.insert(library_dirs, library_dir)
  end
  for library in string.gmatch(libraries_output, "-l([^%s]+)") do
    table.insert(libraries, library)
  end
  for option in string.gmatch(link_options_output or "", "[^%s]+") do
    table.insert(extra_link_options, option)
  end
  print("SDL3: using the system SDL3 package reported by pkg-config")
else
  local package_root = os.getenv("SDL3_ROOT")
  if not package_root or package_root == "" then
    error(
        "SDL3_ROOT must identify an SDL3 development package for desktop " ..
        "Premake builds. The repository CMake build uses the pinned " ..
        "third_party/SDL3 source directly.")
  end
  local package_include_dir = path.join(package_root, "include")
  local package_library_dir =
      os.getenv("SDL3_LIBRARY_DIR") or path.join(package_root, "lib")
  local library_name = os.getenv("SDL3_LIBRARY_NAME")
  if not library_name or library_name == "" then
    library_name = os.istarget("windows") and "SDL3-static" or "SDL3"
  end
  if not os.isfile(path.join(package_include_dir, "SDL3", "SDL.h")) then
    error("SDL3_ROOT does not contain include/SDL3/SDL.h: " .. package_root)
  end
  include_dirs = {package_include_dir}
  library_dirs = {package_library_dir}
  libraries = {library_name}
  if os.istarget("windows") then
    libraries = table.join(libraries, {
      "kernel32",
      "user32",
      "gdi32",
      "winmm",
      "imm32",
      "ole32",
      "oleaut32",
      "version",
      "uuid",
      "advapi32",
      "setupapi",
      "shell32",
      "dinput8",
    })
  elseif os.istarget("macosx") then
    libraries = table.join(libraries, {
      "m",
      "CoreMedia.framework",
      "CoreVideo.framework",
      "Cocoa.framework",
      "IOKit.framework",
      "ForceFeedback.framework",
      "Carbon.framework",
      "CoreAudio.framework",
      "AudioToolbox.framework",
      "AVFoundation.framework",
      "Foundation.framework",
      "GameController.framework",
      "Metal.framework",
      "QuartzCore.framework",
    })
    extra_link_options = {
      "-lpthread",
      "-Wl,-weak_framework,UniformTypeIdentifiers",
      "-Wl,-weak_framework,CoreHaptics",
    }
  end
  print("SDL3: using desktop development package " .. package_root)
end

function sdl3_include()
  if os.istarget("linux") then
    -- premake-cmake does not currently emit externalincludedirs, so use a
    -- normal target include path for the Linux CMake generator.
    includedirs(include_dirs)
  else
    externalincludedirs(include_dirs)
  end
end

function sdl3_link()
  sdl3_include()
  libdirs(library_dirs)
  links(libraries)
  linkoptions(extra_link_options)
end
