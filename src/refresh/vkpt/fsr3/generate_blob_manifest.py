#!/usr/bin/env python3
"""Assemble the reproducible Q2RTX FSR3 blob manifest after shader builds."""

import json
import pathlib
import sys


def main():
    if len(sys.argv) < 8 or (len(sys.argv) - 2) % 6:
        raise SystemExit("usage: generate_blob_manifest.py OUTPUT --row NAME PASS OPTIONS SPIRV METADATA ...")
    output = pathlib.Path(sys.argv[1])
    args = sys.argv[2:]
    rows = []
    while args:
        if args.pop(0) != "--row":
            raise SystemExit("expected --row")
        name, pass_id, options, spirv, metadata = (args.pop(0) for _ in range(5))
        reflected = json.loads(pathlib.Path(metadata).read_text(encoding="utf-8"))
        bindings = {kind: [] for kind in
                    ("cbv", "srvTextures", "uavTextures", "srvBuffers", "uavBuffers", "samplers")}
        for resource in reflected.get("resources", []):
            kind = resource.get("kind")
            if kind in bindings:
                bindings[kind].append({"name": resource["name"],
                                       "binding": resource["binding"],
                                       "count": 1, "space": 0})
        if not reflected.get("resources"):
            raise SystemExit(f"{metadata}: shader reflection contains no resources")
        rows.append({"name": name, "pass": int(pass_id), "options": int(options),
                     "spirv": str(pathlib.Path(spirv).resolve()), "entry": "CS",
                     "bindings": bindings})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"version": 1, "permutations": rows},
                                 indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
