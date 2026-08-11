#!/bin/sh

set -eu

cmake_version="4.3.3"
cmake_sha256="5221a13450c7a0219a2a0d1b6c9085eb06489721fafd8488ccebc1584175d2fb"
ninja_version="1.13.2"
ninja_sha256="c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b"

repository_path="${CI_PRIMARY_REPOSITORY_PATH:-$(
  CDPATH= cd -- "$(dirname -- "$0")/.." && pwd
)}"
derived_data_path="${CI_DERIVED_DATA_PATH:-${TMPDIR:-/tmp}/xenios-xcode-cloud}"
tools_path="${derived_data_path}/xenios-ci-tools"

timestamp() {
  printf '[%s] %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*"
}

retry() {
  attempts=0
  until "$@"; do
    attempts=$((attempts + 1))
    if [ "${attempts}" -ge 5 ]; then
      return 1
    fi
    sleep 5
  done
}

download_asset() {
  url="$1"
  destination="$2"
  expected_sha256="$3"
  temporary="${destination}.download"

  if [ -f "${destination}" ] &&
      printf '%s  %s\n' "${expected_sha256}" "${destination}" |
        shasum -a 256 -c - >/dev/null 2>&1; then
    return 0
  fi

  rm -f "${destination}" "${temporary}"
  retry curl --fail --location --retry 3 --silent --show-error \
    "${url}" --output "${temporary}"
  printf '%s  %s\n' "${expected_sha256}" "${temporary}" |
    shasum -a 256 -c -
  mv "${temporary}" "${destination}"
}

mkdir -p "${tools_path}"

cmake_archive="${tools_path}/cmake-${cmake_version}-macos-universal.tar.gz"
cmake_root="${tools_path}/cmake-${cmake_version}-macos-universal"
cmake_path="${cmake_root}/CMake.app/Contents/bin/cmake"
if [ ! -x "${cmake_path}" ]; then
  timestamp "Provisioning CMake ${cmake_version}"
  download_asset \
    "https://github.com/Kitware/CMake/releases/download/v${cmake_version}/cmake-${cmake_version}-macos-universal.tar.gz" \
    "${cmake_archive}" \
    "${cmake_sha256}"
  rm -rf "${cmake_root}"
  tar -xzf "${cmake_archive}" -C "${tools_path}"
fi

ninja_archive="${tools_path}/ninja-mac-${ninja_version}.zip"
ninja_root="${tools_path}/ninja-${ninja_version}"
ninja_path="${ninja_root}/ninja"
if [ ! -x "${ninja_path}" ]; then
  timestamp "Provisioning Ninja ${ninja_version}"
  download_asset \
    "https://github.com/ninja-build/ninja/releases/download/v${ninja_version}/ninja-mac.zip" \
    "${ninja_archive}" \
    "${ninja_sha256}"
  rm -rf "${ninja_root}"
  mkdir -p "${ninja_root}"
  unzip -q "${ninja_archive}" -d "${ninja_root}"
  chmod u+x "${ninja_path}"
fi

PATH="$(dirname "${cmake_path}"):$(dirname "${ninja_path}"):${PATH}"
export PATH

cd "${repository_path}"
: "${CI_TEAM_ID:?Xcode Cloud did not provide CI_TEAM_ID}"
: "${CI_BUNDLE_ID:?Xcode Cloud did not provide CI_BUNDLE_ID}"

timestamp "Validating the Xcode Cloud iPhoneOS environment"
test "$(uname -m)" = "arm64"
xcodebuild -version
xcodebuild -version | grep -q '^Xcode 27\.'
sdk_version="$(xcrun --sdk iphoneos --show-sdk-version)"
case "${sdk_version}" in
  27.*) ;;
  *)
    echo "ERROR: Xcode Cloud requires iPhoneOS SDK 27.x, got ${sdk_version}"
    exit 1
    ;;
esac
xcrun --sdk iphoneos --show-sdk-path | grep -q '/iPhoneOS.platform/'
xcrun --sdk iphoneos --find clang

if ! xcrun --find metal >/dev/null 2>&1 ||
    ! xcrun --find metallib >/dev/null 2>&1; then
  timestamp "Provisioning the Xcode Metal toolchain"
  retry xcodebuild -downloadComponent MetalToolchain
fi
xcrun --find metal
xcrun --find metallib
"${cmake_path}" --version
"${ninja_path}" --version

timestamp "Initializing repository submodules"
git submodule sync --recursive
retry git -c fetch.recurseSubmodules=on-demand submodule update \
  --init --checkout --recursive --depth=1 --jobs 4

timestamp "Building the native arm64 Premake host tool"
macos_sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
env \
  -u ARCHS \
  -u CFLAGS \
  -u CPPFLAGS \
  -u CXXFLAGS \
  -u EFFECTIVE_PLATFORM_NAME \
  -u IPHONEOS_DEPLOYMENT_TARGET \
  -u LDFLAGS \
  -u PLATFORM_NAME \
  -u TARGETED_DEVICE_FAMILY \
  CC="$(xcrun --sdk macosx --find clang)" \
  CXX="$(xcrun --sdk macosx --find clang++)" \
  SDKROOT="${macos_sdk_path}" \
  python3 tools/build/premake.py --version
premake_host="third_party/premake-core/bin/release/premake5"
test -x "${premake_host}"
file "${premake_host}" | grep -q 'Mach-O 64-bit executable arm64'

export XE_TARGET_IOS=1
export IPHONEOS_DEPLOYMENT_TARGET=16.0

timestamp "Generating 86 iPhoneOS Metal shader headers"
env \
  -u ARCHS \
  -u CC \
  -u CFLAGS \
  -u CPPFLAGS \
  -u CXX \
  -u CXXFLAGS \
  -u EFFECTIVE_PLATFORM_NAME \
  -u LDFLAGS \
  -u PLATFORM_NAME \
  -u SDKROOT \
  -u TARGETED_DEVICE_FAMILY \
  ./xb buildshaders --target metal --target_os ios

timestamp "Preparing SDL3, embedded assets, and the iPhoneOS Xcode graph"
./xb setup --target-os=ios

test -s build/xenia.xcworkspace/contents.xcworkspacedata
test -s build/xenia-app.xcodeproj/project.pbxproj

timestamp "Switching the stable Cloud workspace to the generated project"
python3 - <<'PY'
import os
from pathlib import Path

bootstrap = "XcodeCloudBootstrap/xenia-app.xcodeproj"
generated = "build/xenia-app.xcodeproj"
references = (
    (Path("XeniOS.xcworkspace/contents.xcworkspacedata"), 1),
    (Path("XeniOS.xcworkspace/xcshareddata/xcschemes/xenia-app.xcscheme"), 3),
)
candidates = {}

for path, expected_count in references:
    contents = path.read_text(encoding="utf-8")
    bootstrap_count = contents.count(bootstrap)
    generated_count = contents.count(generated)
    if bootstrap_count == expected_count and generated_count == 0:
        candidate = contents.replace(bootstrap, generated)
    elif bootstrap_count == 0 and generated_count == expected_count:
        candidate = contents
    else:
        raise SystemExit(
            f"ERROR: unexpected Xcode Cloud references in {path}: "
            f"bootstrap={bootstrap_count}, generated={generated_count}, "
            f"expected={expected_count}"
        )
    if candidate.count(bootstrap) != 0 or candidate.count(generated) != expected_count:
        raise SystemExit(f"ERROR: failed to retarget Xcode Cloud references in {path}")
    candidates[path] = candidate

temporaries = []
try:
    for path, candidate in candidates.items():
        if path.read_text(encoding="utf-8") == candidate:
            continue
        temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
        temporary.write_text(candidate, encoding="utf-8")
        temporaries.append(temporary)
        os.replace(temporary, path)
finally:
    for temporary in temporaries:
        temporary.unlink(missing_ok=True)
PY

workspace_list="${derived_data_path}/xenios-xcode-cloud-workspace.json"
build_settings="${derived_data_path}/xenios-xcode-cloud-build-settings.txt"
xcodebuild -list -json -workspace XeniOS.xcworkspace > "${workspace_list}"
xcodebuild \
  -workspace XeniOS.xcworkspace \
  -scheme xenia-app \
  -configuration Release \
  -sdk iphoneos \
  -destination 'generic/platform=iOS' \
  -showBuildSettings > "${build_settings}"

python3 - "${workspace_list}" "${build_settings}" <<'PY'
import json
import os
from pathlib import Path
import sys

workspace = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
schemes = workspace.get("workspace", {}).get("schemes", [])
if "xenia-app" not in schemes:
    raise SystemExit("ERROR: generated Cloud workspace lacks xenia-app")

settings = {}
for line in Path(sys.argv[2]).read_text(encoding="utf-8").splitlines():
    if line.startswith("    ") and " = " in line:
        key, value = line[4:].split(" = ", 1)
        settings[key.strip()] = value.strip()

expected = {
    "CODE_SIGN_STYLE": "Automatic",
    "DEVELOPMENT_TEAM": os.environ["CI_TEAM_ID"],
    "FULL_PRODUCT_NAME": "XeniOS.app",
    "IPHONEOS_DEPLOYMENT_TARGET": "16.0",
    "PRODUCT_BUNDLE_IDENTIFIER": os.environ["CI_BUNDLE_ID"],
    "PRODUCT_NAME": "XeniOS",
    "PROJECT_FILE_PATH": str(Path.cwd() / "build/xenia-app.xcodeproj"),
    "TARGET_NAME": "xenia-app",
}
for key, value in expected.items():
    if settings.get(key) != value:
        raise SystemExit(
            f"ERROR: unexpected {key}: {settings.get(key)!r} != {value!r}"
        )
if "arm64" not in settings.get("ARCHS", "").split():
    raise SystemExit(f"ERROR: generated ARCHS is {settings.get('ARCHS')!r}")
sdkroot = settings.get("SDKROOT", "")
if "iPhoneOS" not in sdkroot or "Simulator" in sdkroot:
    raise SystemExit(f"ERROR: generated SDKROOT is not iPhoneOS: {sdkroot!r}")
PY

header_count="$(
  find \
    build/generated/xenia/gpu/shaders/bytecode/metal \
    build/generated/xenia/ui/shaders/bytecode/metal \
    -type f -name '*.h' -print |
    wc -l |
    tr -d '[:space:]'
)"
test "${header_count}" -eq 86

timestamp "Xcode Cloud bootstrap completed successfully"
