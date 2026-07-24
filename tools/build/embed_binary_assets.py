#!/usr/bin/env python3
"""Generate header-only byte arrays for repository assets."""

import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: embed_binary_assets.py SOURCE OUTPUT_DIR NAMESPACE", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    namespace = sys.argv[3]
    declarations = []
    for path in sorted(item for item in source.iterdir() if item.is_file()):
        symbol = re.sub(r"[^A-Za-z0-9_]", "_", path.name)
        if symbol[0].isdigit():
            symbol = "_" + symbol
        values = ", ".join(f"0x{value:02x}" for value in path.read_bytes())
        declarations.append(
            f"inline constexpr unsigned char {symbol}_data[] = {{ {values} }};\n"
            f"inline constexpr size_t {symbol}_size = sizeof({symbol}_data);"
        )
    output.mkdir(parents=True, exist_ok=True)
    (output / f"embedded_{namespace}.h").write_text(
        "#pragma once\n#include <cstddef>\n"
        f"namespace xe::ui::embedded_{namespace} {{\n"
        + "\n".join(declarations)
        + f"\n}}  // namespace xe::ui::embedded_{namespace}\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
