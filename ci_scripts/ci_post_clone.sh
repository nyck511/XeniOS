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
