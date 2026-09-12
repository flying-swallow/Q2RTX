#!/usr/bin/env python3
"""Compile one fixed FSR3 HLSL permutation and emit glslang reflection JSON."""

import argparse
import json
import re
import subprocess


def restore_atomic_image_formats(spirv_path):
    """Give unsigned-int storage images an explicit R32ui format again.

    --no-storage-format leaves every storage image with format Unknown, which
    is what the float textures need (the SDK allocates them as R16G16 and the
    like).  SPIR-V does, however, require an explicit format for images used
    with atomics; FSR3's only uint UAVs (the SPD counter and the reconstructed
    previous depth) are both R32_UINT resources, so R32ui is exact.
    """
    import struct
    data = bytearray(pathlib_read_bytes(spirv_path))
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    uint_types = set()
    offset = 5
    changed = False
    while offset < len(words):
        word_count = words[offset] >> 16
        opcode = words[offset] & 0xFFFF
        if word_count == 0:
            raise SystemExit(f"{spirv_path}: malformed SPIR-V")
        if opcode == 21 and word_count == 4 and words[offset + 2] == 32 and words[offset + 3] == 0:
            uint_types.add(words[offset + 1])  # OpTypeInt 32 unsigned
        elif opcode == 25 and word_count >= 9:  # OpTypeImage
            sampled_type, sampled, image_format = words[offset + 2], words[offset + 7], words[offset + 8]
            if sampled_type in uint_types and sampled == 2 and image_format == 0:
                words[offset + 8] = 33  # R32ui
                changed = True
        offset += word_count
    if changed:
        with open(spirv_path, "wb") as spirv_file:
            spirv_file.write(struct.pack("<%dI" % len(words), *words))


def pathlib_read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    args = parser.parse_args()

    # HLSL b/t/s/u registers share numbers; keep each class in its own
    # binding range so the descriptor set layout has no collisions.  The
    # backend reads the shifted bindings back from the reflection below.
    command = [args.compiler, "-D", "-S", "comp", "-e", "CS",
               "--target-env", "vulkan1.2", "-q",
               "--shift-UBO-binding", "0",
               "--shift-texture-binding", "1000",
               "--shift-sampler-binding", "2000",
               "--shift-image-binding", "3000",
               "--shift-ssbo-binding", "4000",
               # HLSL RWTexture2D<T> carries no format; let the SPIR-V say
               # Unknown so the SDK's R16/R11G11B10 storage images bind legally
               # (the renderer enables the *WithoutFormat device features).
               "--no-storage-format",
               "-o", args.output]
    for include in args.include:
        command.append("-I" + include)
    for define in args.define:
        command.extend(["--define-macro", define])
    command.append(args.source)
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit(result.returncode)
    restore_atomic_image_formats(args.output)

    resources = []
    in_uniforms = False
    in_uniform_blocks = False
    in_buffer_blocks = False
    for line in result.stdout.splitlines():
        if line == "Uniform reflection:":
            in_uniforms = True
            continue
        if line == "Uniform block reflection:":
            in_uniforms = False
            in_uniform_blocks = True
            continue
        if line == "Buffer block reflection:":
            in_uniforms = False
            in_uniform_blocks = False
            in_buffer_blocks = True
            continue
        if line and not line.startswith(" ") and line.endswith("reflection:"):
            in_uniforms = False
            in_uniform_blocks = False
            in_buffer_blocks = False
        if in_buffer_blocks:
            match = re.match(r"([^:]+): .*binding (-?\d+), stages (\d+)", line)
            if match and int(match.group(2)) >= 0:
                name = match.group(1).strip()
                resources.append({"name": name, "binding": int(match.group(2)),
                                  "stages": int(match.group(3)),
                                  "kind": "uavBuffers" if name.startswith("rw_") else "srvBuffers"})
        if in_uniforms:
            match = re.match(r"([^:]+): .*binding (-?\d+), stages (\d+)", line)
            if match and int(match.group(2)) >= 0:
                name = match.group(1).strip()
                if name.startswith("cb"):
                    kind = "cbv"
                elif name.startswith("rw_"):
                    kind = "uavTextures"
                elif name.startswith("r_"):
                    kind = "srvTextures"
                elif name.startswith("s_"):
                    kind = "samplers"
                else:
                    kind = "srvBuffers"
                resources.append({"name": name, "binding": int(match.group(2)),
                                  "stages": int(match.group(3)), "kind": kind})
        if in_uniform_blocks:
            match = re.match(r"([^:]+): .*binding (-?\d+), stages (\d+)", line)
            if match and int(match.group(2)) >= 0:
                resources.append({"name": match.group(1).strip(),
                                  "binding": int(match.group(2)),
                                  "stages": int(match.group(3)), "kind": "cbv"})

    metadata = {"source": args.source, "entry": "CS", "stage": "compute",
                "target_env": "vulkan1.2", "defines": args.define,
                "resources": resources,
                "glslang_reflection": result.stdout}
    with open(args.metadata, "w", encoding="utf-8") as metadata_file:
        json.dump(metadata, metadata_file, indent=2, sort_keys=True)
        metadata_file.write("\n")


if __name__ == "__main__":
    main()
