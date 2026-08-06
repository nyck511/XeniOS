#!/usr/bin/env bash

set -euo pipefail

readonly max_attempts=5

git submodule sync --recursive

for ((attempt = 1; attempt <= max_attempts; ++attempt)); do
  echo "Submodule update attempt ${attempt}/${max_attempts}"
  if git \
    -c http.lowSpeedLimit=1000 \
    -c http.lowSpeedTime=60 \
    submodule update --init --recursive --jobs 4; then
    break
  fi

  if ((attempt == max_attempts)); then
    echo "::error::Unable to initialize pinned submodules after ${max_attempts} attempts"
    exit 1
  fi

  sleep_seconds=$((attempt * 10))
  echo "Submodule update failed; retrying in ${sleep_seconds} seconds"
  sleep "${sleep_seconds}"
done

submodule_status="$(git submodule status --recursive)"
printf '%s\n' "${submodule_status}"
if grep -Eq '^[-+U]' <<<"${submodule_status}"; then
  echo "::error::One or more submodules are missing or not at the pinned revision"
  exit 1
fi
