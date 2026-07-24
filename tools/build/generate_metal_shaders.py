#!/usr/bin/env python3
"""Generate iOS metallib headers from the repository's Slang shaders."""

import os
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_metal_shaders.py SHADER_CC REPOSITORY", file=sys.stderr)
        return 2
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(sys.argv[2]).resolve()
    environment = dict(os.environ)
    environment.setdefault(
        "SLANGC_PATH", str(repository / ".slang" / "2026.8" / "bin" / "slangc")
    )
    for relative_dir in ("src/xenia/gpu/shaders", "src/xenia/ui/shaders"):
        shader_dir = repository / relative_dir
        output_dir = shader_dir / "bytecode" / "metal"
        output_dir.mkdir(parents=True, exist_ok=True)
        for source in sorted(shader_dir.glob("*.slang")):
            first_line = source.read_text(errors="replace").splitlines()[0]
            if "XE_DXIL_ONLY" in first_line or "XE_NO_MSL" in first_line:
                continue
            stem = source.name.removesuffix(".slang").replace(".", "_")
            if stem[-2:] not in ("vs", "ps", "cs"):
                continue
            output = output_dir / f"{stem}.h"
            subprocess.run(
                [
                    str(compiler),
                    "--slang-msl",
                    "--metal-sdk",
                    "iphoneos",
                    "--metal-std",
                    "ios-metal2.3",
                    "--metal-min-version-flag",
                    "-miphoneos-version-min=17.0",
                    str(source),
                    str(output),
                ],
                check=True,
                env=environment,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
