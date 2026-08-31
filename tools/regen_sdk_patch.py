#!/usr/bin/env python3
"""Regenerate sdk_patches/ from the pinned archive and the working SDK tree.

apply_sdk_patches.py can only APPLY and VERIFY the recorded patch; there was no
way to record a new one, so editing a vendored SDK file meant hand-writing a
unified diff and hand-updating three hashes. This closes that gap.

It is deliberately fail-closed in the same spirit as the applicator:
  * every pristine file must come from the pinned archive and match the
    manifest's recorded pristine_sha256;
  * the regenerated patch is re-applied to the pristine bytes and the result
    must equal the working tree byte-for-byte before anything is written;
  * the manifest is rewritten only after that round-trip succeeds.

Usage:
    python3 tools/regen_sdk_patch.py                 # check only, writes nothing
    python3 tools/regen_sdk_patch.py --write
"""
from __future__ import annotations

import argparse, difflib, hashlib, json, sys, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from apply_sdk_patches import parse_unified_patch, apply_hunks  # the real applier

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "sdk_patches" / "manifest.json"
ARCHIVE = REPO / "build" / "Zigbee_SDK.zip"
SDK_ROOT = REPO / "build" / "tl_zigbee_sdk"


def eol_stripped(blob: bytes) -> list[str]:
    return [l.rstrip("\r\n") for l in blob.decode("utf-8").splitlines(keepends=True)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="write the patch and manifest")
    a = ap.parse_args()

    man = json.loads(MANIFEST.read_text(encoding="utf-8"))
    order = list(parse_unified_patch(MANIFEST.parent / man["patch"]))  # preserve file order
    z = zipfile.ZipFile(ARCHIVE)
    entries = {n.split("tl_zigbee_sdk/", 1)[1]: n
               for n in z.namelist() if "tl_zigbee_sdk/" in n and not n.endswith("/")}

    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != man["sdk_archive_sha256"]:
        print("FAIL: pinned SDK archive hash does not match the manifest", file=sys.stderr)
        return 1

    pristine: dict[str, bytes] = {}
    chunks: list[str] = []
    for rel in order:
        want = man["files"][rel]["pristine_sha256"]
        blob = z.read(entries[rel])
        if hashlib.sha256(blob).hexdigest() != want:
            print(f"FAIL: {rel} in the archive is not the recorded pristine file", file=sys.stderr)
            return 1
        pristine[rel] = blob
        cur = (SDK_ROOT / rel).read_bytes()
        diff = list(difflib.unified_diff(
            eol_stripped(blob), eol_stripped(cur),
            fromfile=f"a/tl_zigbee_sdk/{rel}", tofile=f"b/tl_zigbee_sdk/{rel}",
            lineterm="", n=3))
        if not diff:
            print(f"FAIL: {rel} is unmodified; a recorded file must differ", file=sys.stderr)
            return 1
        chunks.append("\n".join(diff))

    patch_text = "\n".join(chunks) + "\n"
    tmp = MANIFEST.parent / (man["patch"] + ".regen")
    tmp.write_text(patch_text, encoding="utf-8")
    try:
        parsed = parse_unified_patch(tmp)
        for rel in order:
            produced = apply_hunks(pristine[rel], parsed[rel], rel)
            actual = (SDK_ROOT / rel).read_bytes()
            if produced != actual:
                print(f"FAIL: round-trip mismatch for {rel} - patch does not reproduce the tree",
                      file=sys.stderr)
                return 1
            man["files"][rel]["patched_sha256"] = hashlib.sha256(actual).hexdigest()
        print(f"round-trip OK for all {len(order)} files")
    finally:
        if not a.write:
            tmp.unlink(missing_ok=True)

    if not a.write:
        print("check only; re-run with --write to record")
        return 0

    tmp.replace(MANIFEST.parent / man["patch"])
    man["patch_sha256"] = hashlib.sha256((MANIFEST.parent / man["patch"]).read_bytes()).hexdigest()
    MANIFEST.write_text(json.dumps(man, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {man['patch']} and manifest.json")
    return 0


sys.exit(main())
