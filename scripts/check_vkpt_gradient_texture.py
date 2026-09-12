#!/usr/bin/env python3
"""Reject references to the removed, unproduced ASVGF gradient texture."""

import pathlib
import sys


RESOURCE = "ASVGF_GRAD_SMPL_POS_A"
CONSUMER = "TEX_ASVGF_GRAD_SMPL_POS_A"


def main() -> int:
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else pathlib.Path(__file__).parents[1]
    shader_dir = root / "src" / "refresh" / "vkpt" / "shader"
    files = [shader_dir / "global_textures.h"]
    files.extend(sorted(shader_dir.glob("*.glsl")))
    files.extend(sorted(shader_dir.glob("*.h")))
    files.extend(sorted(shader_dir.glob("*.comp")))
    files.extend(sorted(shader_dir.glob("*.rgen")))
    files.extend(sorted(shader_dir.glob("*.rchit")))
    files.extend(sorted(shader_dir.glob("*.rahit")))
    files.extend(sorted(shader_dir.glob("*.rmiss")))
    files.extend(sorted(shader_dir.glob("*.rint")))
    files.extend(sorted(shader_dir.glob("*.vert")))
    files.extend(sorted(shader_dir.glob("*.frag")))

    failures = []
    for path in dict.fromkeys(files):
        text = path.read_text(encoding="utf-8")
        for line_number, line in enumerate(text.splitlines(), 1):
            if RESOURCE in line or CONSUMER in line:
                failures.append(f"{path.relative_to(root)}:{line_number}: {line.strip()}")

    if failures:
        print("unproduced ASVGF gradient texture is still referenced:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("removed ASVGF gradient texture has no shader declaration or consumer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
