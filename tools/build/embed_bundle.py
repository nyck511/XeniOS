#!/usr/bin/env python3
"""Generate a header-only compressed EmbeddedBundle from files."""

import pathlib
import struct
import sys
import zlib


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: embed_bundle.py SOURCE OUTPUT_DIR NAMESPACE", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    namespace = sys.argv[3]
    files = [source] if source.is_file() else sorted(
        path for path in source.rglob("*") if path.is_file()
    )
    raw = bytearray(struct.pack("<I", len(files)))
    for path in files:
        name = path.name if source.is_file() else path.relative_to(source).as_posix()
        name_bytes = name.encode()
        data = path.read_bytes()
        raw += struct.pack("<I", len(name_bytes)) + name_bytes
        raw += struct.pack("<I", len(data)) + data
    compressed = zlib.compress(bytes(raw), 9)
    values = ", ".join(f"0x{value:02x}" for value in compressed)
    output.mkdir(parents=True, exist_ok=True)
    header = output / f"embedded_bundle_{namespace}.h"
    header.write_text(
        "#pragma once\n#include <cstddef>\n"
        f"namespace xe::embedded_bundle_{namespace} {{\n"
        f"inline constexpr unsigned char kBundleData[] = {{ {values} }};\n"
        "inline constexpr size_t kBundleSize = sizeof(kBundleData);\n"
        f"}}  // namespace xe::embedded_bundle_{namespace}\n"
    )
    # CMake lists this file as a source; Premake uses the inline header only.
    (output / f"embedded_bundle_{namespace}.cc").write_text(
        f'#include "embedded_bundle_{namespace}.h"\n'
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
