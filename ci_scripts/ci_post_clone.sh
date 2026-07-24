#!/bin/sh
set -eu

cmake_version="4.3.3"
cmake_sha256="5221a13450c7a0219a2a0d1b6c9085eb06489721fafd8488ccebc1584175d2fb"
repository_path="${CI_PRIMARY_REPOSITORY_PATH:-$(pwd)}"
tools_path="${CI_DERIVED_DATA_PATH:-${TMPDIR:-/tmp}}/xenios-ci-tools"
archive_path="${tools_path}/cmake-${cmake_version}-macos-universal.tar.gz"
cmake_path="${tools_path}/cmake-${cmake_version}-macos-universal/CMake.app/Contents/bin/cmake"

mkdir -p "${tools_path}"

if [ ! -x "${cmake_path}" ]; then
  curl --fail --location --retry 3 \
    "https://github.com/Kitware/CMake/releases/download/v${cmake_version}/cmake-${cmake_version}-macos-universal.tar.gz" \
    --output "${archive_path}"
  printf '%s  %s\n' "${cmake_sha256}" "${archive_path}" | shasum -a 256 -c -
  tar -xzf "${archive_path}" -C "${tools_path}"
fi

"${cmake_path}" \
  -S "${repository_path}/third_party/zlib-ng" \
  -B "${repository_path}/third_party/zlib-ng" \
  -DZLIB_ENABLE_TESTS=OFF \
  -DWITH_GTEST=OFF

cd "${repository_path}"
data_path="${repository_path}/build/data_repos"
rm -rf "${data_path}"
mkdir -p "${data_path}"
git clone --depth=1 --branch main \
  https://github.com/xenia-canary/game-patches.git \
  "${data_path}/game-patches"
git clone --depth=1 --branch main \
  https://github.com/xenia-manager/database.git \
  "${data_path}/xenia-manager-database"
git clone --depth=1 --branch main \
  https://github.com/xenia-manager/x360db.git \
  "${data_path}/x360db"
git clone --depth=1 --branch master \
  https://github.com/mdqinc/SDL_GameControllerDB.git \
  "${data_path}/SDL_GameControllerDB"
python3 tools/build/embed_bundle.py \
  build/data_repos/xenia-manager-database/data/game-compatibility \
  src/xenia/app game_compat
python3 tools/build/embed_bundle.py \
  build/data_repos/x360db/games.json \
  src/xenia/app game_db
python3 tools/build/embed_bundle.py \
  build/data_repos/game-patches/patches \
  src/xenia/patcher patches
python3 tools/build/embed_bundle.py \
  build/data_repos/SDL_GameControllerDB/gamecontrollerdb.txt \
  src/xenia/hid/sdl gamecontrollerdb
python3 tools/build/embed_binary_assets.py \
  assets/font src/xenia/ui font
python3 tools/build/embed_binary_assets.py \
  assets/icon src/xenia/ui app_icon
python3 tools/build/embed_binary_assets.py \
  assets/icons src/xenia/app icons

python3 -c \
  'import runpy; runpy.run_path("xenia-build.py", run_name="xenia_build_module")["generate_version_h"]()'
python3 -c \
  'import runpy; runpy.run_path("xenia-build.py", run_name="xenia_build_module")["download_slang"]()'

host_build_path="${CI_DERIVED_DATA_PATH:-${TMPDIR:-/tmp}}/xenios-host-tools"
"${cmake_path}" -S "${repository_path}" -B "${host_build_path}" \
  -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DXENIA_BUILD_TESTS=OFF \
  -DXENIA_BUILD_MISC=OFF
"${cmake_path}" --build "${host_build_path}" \
  --target xenia-shader-cc --parallel 8
mkdir -p "${repository_path}/build/host_tools/Release"
cp "${host_build_path}/host_tools/xenia-shader-cc" \
  "${repository_path}/build/host_tools/Release/xenia-shader-cc"
python3 tools/build/generate_metal_shaders.py \
  "${host_build_path}/host_tools/xenia-shader-cc" \
  "${repository_path}"

ios_build_path="${repository_path}/build-cmake-ios"
"${cmake_path}" -S "${repository_path}" -B "${ios_build_path}" \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DXENIA_BUILD_TESTS=OFF \
  -DXENIA_BUILD_MISC=OFF

# The Cloud workflow is attached to this workspace path. Point it at the
# CMake-generated project whose xenia-app scheme matches the current SDL3
# source tree.
python3 -c \
  'from pathlib import Path; p = Path("build/xenia.xcworkspace/contents.xcworkspacedata"); p.write_text("""<?xml version="1.0" encoding="UTF-8"?>
<Workspace version="1.0">
  <FileRef location="group:../build-cmake-ios/xenia.xcodeproj"/>
</Workspace>
""")'
