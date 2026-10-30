#!/usr/bin/env python3
"""Verify an Android APK contains the complete scenes tree and arm64 native runtime."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

AARCH64 = 183
REQUIRED_LIBS = (
    "lib/arm64-v8a/libmain.so",
    "lib/arm64-v8a/libSDL2.so",
    "lib/arm64-v8a/libwgpu_native.so",
)
INDEX_PATH = "assets/maque/scenes.json"


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def scene_files(root: Path) -> list[str]:
    return sorted(
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    )


def verify_elf_aarch64(name: str, data: bytes) -> None:
    if len(data) < 20 or data[:4] != b"\x7fELF":
        fail(f"{name} is not an ELF file")
    if data[4] != 2 or data[5] != 1:
        fail(f"{name} is not a little-endian ELF64 file")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != AARCH64:
        fail(f"{name} machine={machine}, expected AArch64 ({AARCH64})")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("apk", type=Path)
    parser.add_argument("scenes", type=Path)
    args = parser.parse_args()

    if not args.apk.is_file():
        fail(f"APK not found: {args.apk}")
    if not args.scenes.is_dir():
        fail(f"scenes directory not found: {args.scenes}")

    source_files = scene_files(args.scenes)
    expected_assets = {f"assets/{name}" for name in source_files}
    expected_scenes = sorted(name for name in source_files if name.endswith(".scn"))

    with zipfile.ZipFile(args.apk) as apk:
        names = {name for name in apk.namelist() if not name.endswith("/")}
        packaged_assets = {name for name in names if name.startswith("assets/")}
        actual_scene_assets = packaged_assets - {INDEX_PATH}

        missing = sorted(expected_assets - actual_scene_assets)
        unexpected = sorted(actual_scene_assets - expected_assets)
        if missing:
            fail(f"APK is missing {len(missing)} scene asset(s): {missing[:10]}")
        if unexpected:
            fail(f"APK has {len(unexpected)} unexpected scene asset(s): {unexpected[:10]}")

        if INDEX_PATH not in names:
            fail(f"generated index missing: {INDEX_PATH}")
        index = json.loads(apk.read(INDEX_PATH).decode("utf-8"))
        if index.get("version") != 1:
            fail(f"scene index version is {index.get('version')!r}, expected 1")
        if index.get("scenes") != expected_scenes:
            fail("scene index does not exactly match sorted scenes/**/*.scn")

        for library in REQUIRED_LIBS:
            if library not in names:
                fail(f"native library missing: {library}")
            verify_elf_aarch64(library, apk.read(library))

    print(
        "OK: "
        f"{len(source_files)} scene asset files, "
        f"{len(expected_scenes)} .scn entries, "
        f"{len(REQUIRED_LIBS)} AArch64 native libraries"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
