#!/usr/bin/env python3
"""Fail-closed applicator for the pinned Telink SDK modifications.

The extracted SDK is deliberately ignored by git.  This tool only accepts the
five recorded pristine files or the five recorded patched files: mixed,
missing, or locally edited trees are rejected before a target is compiled.
It implements the small unified patch itself so the Windows extraction flow
does not depend on a separately installed ``patch`` executable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Dict, Iterable, List, Sequence, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "sdk_patches" / "manifest.json"
HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class PatchError(RuntimeError):
    """A recorded SDK patch cannot safely be applied or verified."""


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PatchError(f"cannot read SDK patch manifest {path}: {exc}") from exc

    if manifest.get("format") != 1 or not manifest.get("files"):
        raise PatchError(f"unsupported or incomplete SDK patch manifest: {path}")

    patch_path = path.parent / manifest.get("patch", "")
    if not patch_path.is_file():
        raise PatchError(f"recorded SDK patch is missing: {patch_path}")
    expected_patch_hash = manifest.get("patch_sha256")
    if sha256_file(patch_path) != expected_patch_hash:
        raise PatchError(
            f"recorded SDK patch hash mismatch: {patch_path}; restore the tracked patch"
        )
    return manifest


def sdk_state(sdk_root: Path, files: Dict[str, dict]) -> Dict[str, str]:
    states: Dict[str, str] = {}
    for relative, hashes in files.items():
        path = sdk_root / relative
        if not path.is_file():
            states[relative] = "missing"
            continue
        digest = sha256_file(path)
        if digest == hashes["pristine_sha256"]:
            states[relative] = "pristine"
        elif digest == hashes["patched_sha256"]:
            states[relative] = "patched"
        else:
            states[relative] = f"unknown ({digest})"
    return states


def state_summary(states: Dict[str, str]) -> str:
    return "; ".join(f"{path}: {state}" for path, state in sorted(states.items()))


def content_without_eol(line: bytes) -> bytes:
    if line.endswith(b"\r\n"):
        return line[:-2]
    if line.endswith(b"\n") or line.endswith(b"\r"):
        return line[:-1]
    return line


def parse_unified_patch(patch_path: Path) -> Dict[str, List[Tuple[int, int, int, int, List[Tuple[str, str]]]]]:
    """Parse the deliberately small, ordinary unified patch we track."""
    lines = patch_path.read_text(encoding="utf-8").splitlines()
    result: Dict[str, List[Tuple[int, int, int, int, List[Tuple[str, str]]]]] = {}
    index = 0
    while index < len(lines):
        if not lines[index].startswith("--- a/"):
            index += 1
            continue
        old_name = lines[index][6:]
        index += 1
        if index >= len(lines) or not lines[index].startswith("+++ b/"):
            raise PatchError(f"malformed unified patch near {old_name}")
        new_name = lines[index][6:]
        if old_name != new_name:
            raise PatchError(f"rename patches are intentionally unsupported: {old_name} -> {new_name}")
        # The patch is rooted at the archive's tl_zigbee_sdk/ directory;
        # --sdk-root points at that directory itself.
        if old_name.startswith("tl_zigbee_sdk/"):
            old_name = old_name[len("tl_zigbee_sdk/"):]
        index += 1
        hunks: List[Tuple[int, int, int, int, List[Tuple[str, str]]]] = []
        while index < len(lines) and not lines[index].startswith("--- a/"):
            if not lines[index].startswith("@@ "):
                index += 1
                continue
            match = HUNK_RE.match(lines[index])
            if not match:
                raise PatchError(f"malformed hunk header in {old_name}: {lines[index]}")
            old_start = int(match.group(1))
            old_count = int(match.group(2) or "1")
            new_start = int(match.group(3))
            new_count = int(match.group(4) or "1")
            index += 1
            hunk_lines: List[Tuple[str, str]] = []
            while index < len(lines) and not lines[index].startswith("@@ ") and not lines[index].startswith("--- a/"):
                line = lines[index]
                if line.startswith("\\ No newline at end of file"):
                    raise PatchError("patches without a final newline are intentionally unsupported")
                if not line or line[0] not in " +-":
                    raise PatchError(f"malformed hunk body in {old_name}: {line!r}")
                hunk_lines.append((line[0], line[1:]))
                index += 1
            seen_old = sum(kind in " -" for kind, _ in hunk_lines)
            seen_new = sum(kind in " +" for kind, _ in hunk_lines)
            if seen_old != old_count:
                raise PatchError(f"old line count mismatch in {old_name}: expected {old_count}, got {seen_old}")
            if seen_new != new_count:
                raise PatchError(f"new line count mismatch in {old_name}: expected {new_count}, got {seen_new}")
            hunks.append((old_start, old_count, new_start, new_count, hunk_lines))
        result[old_name] = hunks
    return result


def apply_hunks(original: bytes, hunks: Sequence[Tuple[int, int, int, int, List[Tuple[str, str]]]], relative: str) -> bytes:
    """Apply a unified patch while retaining the SDK's line ending convention."""
    lines = original.splitlines(keepends=True)
    added_line_ending = b"\r\n" if any(line.endswith(b"\r\n") for line in lines) else b"\n"
    delta = 0
    for old_start, old_count, _new_start, new_count, hunk_lines in hunks:
        at = old_start - 1 + delta
        if at < 0 or at + old_count > len(lines):
            raise PatchError(f"hunk position outside {relative}")
        original_slice = lines[at:at + old_count]
        consumed = 0
        replacement: List[bytes] = []
        for kind, text in hunk_lines:
            encoded = text.encode("utf-8")
            if kind in " -":
                if consumed >= len(original_slice) or content_without_eol(original_slice[consumed]) != encoded:
                    raise PatchError(f"hunk context mismatch in {relative} at source line {old_start + consumed}")
                current = original_slice[consumed]
                consumed += 1
                if kind == " ":
                    replacement.append(current)
            elif kind == "+":
                replacement.append(encoded + added_line_ending)
        if consumed != old_count:
            raise PatchError(f"hunk accounting mismatch in {relative}")
        lines[at:at + old_count] = replacement
        delta += new_count - old_count
    return b"".join(lines)


def apply_recorded_patch(sdk_root: Path, manifest_path: Path, manifest: dict) -> None:
    files: Dict[str, dict] = manifest["files"]
    parsed = parse_unified_patch(manifest_path.parent / manifest["patch"])
    if set(parsed) != set(files):
        raise PatchError("patch files and manifest files differ; restore both tracked artifacts")

    replacements: Dict[Path, bytes] = {}
    for relative, hashes in files.items():
        source = sdk_root / relative
        patched = apply_hunks(source.read_bytes(), parsed[relative], relative)
        digest = hashlib.sha256(patched).hexdigest()
        if digest != hashes["patched_sha256"]:
            raise PatchError(
                f"patch output hash mismatch for {relative}: expected {hashes['patched_sha256']}, got {digest}"
            )
        replacements[source] = patched

    # Compute every result before modifying any SDK source.
    for path, contents in replacements.items():
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
            temporary.write(contents)
            temp_path = Path(temporary.name)
        os.replace(temp_path, path)


def ota_erase_end(image_size: int, base_address: int = 0x70000,
                  sector_size: int = 0x1000, max_size: int = 0x68000) -> int:
    """Model the patched C boundary calculation for its host regression test."""
    if image_size < 0 or image_size > max_size:
        raise ValueError("OTA image is outside the configured staging capacity")
    sectors = (image_size + sector_size - 1) // sector_size
    end = base_address + sectors * sector_size
    if end < base_address or end > base_address + max_size:
        raise ValueError("OTA erase would cross the configured staging bank")
    return end


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", required=True, type=Path, help="extracted tl_zigbee_sdk directory")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--verify-only", action="store_true", help="reject pristine SDK instead of patching it")
    args = parser.parse_args(argv)

    try:
        manifest_path = args.manifest.resolve()
        manifest = load_manifest(manifest_path)
        sdk_root = args.sdk_root.resolve()
        states = sdk_state(sdk_root, manifest["files"])
        values = set(states.values())
        if values == {"patched"}:
            print(f"MOES SDK patch set verified: {sdk_root}")
            return 0
        if args.verify_only:
            raise PatchError(
                "TS0505B requires the exact recorded MOES SDK patch set; "
                f"refusing unpatched/mixed SDK ({state_summary(states)})"
            )
        if values != {"pristine"}:
            raise PatchError(
                "SDK is mixed, missing, or locally modified; refusing to patch it in place "
                f"({state_summary(states)}). Re-extract the pinned archive."
            )
        apply_recorded_patch(sdk_root, manifest_path, manifest)
        final_states = sdk_state(sdk_root, manifest["files"])
        if set(final_states.values()) != {"patched"}:
            raise PatchError(f"post-apply SDK verification failed ({state_summary(final_states)})")
        print(f"MOES SDK patch set applied and verified: {sdk_root}")
        return 0
    except PatchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
