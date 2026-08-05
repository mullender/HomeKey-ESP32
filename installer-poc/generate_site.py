#!/usr/bin/env python3
"""Generate the multi-board POC Pages site from a build_release.py bundle.

Contract (see installer-poc/README.md):
  * exactly the four named profiles, each exactly once
  * release tag must be `multi-board-poc-<manifest commit_short>`
  * only factory bins are consumed; every factory needs a .sha256
    sidecar whose full text equals "<hash>  <filename>\\n"; every
    factory must be exactly 4 MiB (test fixtures may override)
  * output layout:
        firmware/<factory>
        firmware/<factory>.sha256
        manifests/<profile>.json      (parts.path = ../firmware/<name>)
        index.html                    (static, copied from site/)
        .nojekyll
        build-manifest.json
  * `--out` must NOT exist. The site is built in a sibling temp
    directory and renamed on full success; the temp is removed on
    failure so no partial bundle is ever visible.
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path


# Immutable profile contract. If you add/rename a profile, update this
# map in the same commit as profiles.json / the site template.
REQUIRED_PROFILES = {
    "esp32-generic": {
        "chip_family": "ESP32", "idf_target": "esp32",
        "installer_flag": False, "display_name": "ESP32 generic",
    },
    "esp32s3-generic": {
        "chip_family": "ESP32-S3", "idf_target": "esp32s3",
        "installer_flag": False, "display_name": "ESP32-S3 generic",
    },
    "esp32c3-generic": {
        "chip_family": "ESP32-C3", "idf_target": "esp32c3",
        "installer_flag": False, "display_name": "ESP32-C3 generic",
    },
    "atoms3lite-unit-nfc": {
        "chip_family": "ESP32-S3", "idf_target": "esp32s3",
        "installer_flag": True,
        "display_name": "M5Stack AtomS3 Lite + Unit NFC",
    },
}

DEFAULT_FACTORY_SIZE = 4 * 1024 * 1024
SITE_TEMPLATE_DIR = Path(__file__).resolve().parent / "site"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _load_and_validate_bundle(bundle: Path, release_tag: str,
                              expected_factory_size: int) -> dict:
    manifest_path = bundle / "manifest.json"
    m = json.loads(manifest_path.read_text())
    if m.get("schema_version") != 1:
        raise SystemExit(f"{manifest_path}: unsupported schema_version")

    # Commit short must be a prefix of the full commit.
    if not m["commit"].startswith(m["commit_short"]):
        raise SystemExit(
            f"manifest.commit {m['commit']!r} does not start with commit_short {m['commit_short']!r}")

    # Release tag pinning: prevents a stale bundle being deployed under a
    # tag that points at a different commit.
    expected_tag = f"multi-board-poc-{m['commit_short']}"
    if release_tag != expected_tag:
        raise SystemExit(
            f"release tag mismatch: got {release_tag!r}, expected {expected_tag!r}")

    # Profile-set enforcement.
    seen = {}
    for b in m["builds"]:
        name = b["profile"]
        if name not in REQUIRED_PROFILES:
            raise SystemExit(f"unexpected profile in bundle: {name}")
        if name in seen:
            raise SystemExit(f"profile appears more than once: {name}")
        contract = REQUIRED_PROFILES[name]
        if b["chip_family"] != contract["chip_family"]:
            raise SystemExit(f"{name}: chip_family {b['chip_family']!r} "
                             f"!= expected {contract['chip_family']!r}")
        if b.get("idf_target") != contract["idf_target"]:
            raise SystemExit(f"{name}: idf_target {b.get('idf_target')!r} "
                             f"!= expected {contract['idf_target']!r}")
        # Strict identity check: no truthy coercion. `0`, `1`, `"true"` all
        # fail here, only real JSON booleans compare with `is`.
        if b["installer_flag"] is not contract["installer_flag"]:
            raise SystemExit(f"{name}: installer_flag {b['installer_flag']!r} "
                             f"!= expected {contract['installer_flag']!r}")
        seen[name] = b
    missing = set(REQUIRED_PROFILES) - set(seen)
    if missing:
        raise SystemExit(f"bundle is missing required profiles: {sorted(missing)}")

    # Factory + sidecar validation.
    short = m["commit_short"]
    for b in m["builds"]:
        p = b["profile"]
        contract = REQUIRED_PROFILES[p]
        fac_name = b["factory"]["name"]
        expected_name = f"HomeKey-ESP32-{short}-{p}-{contract['idf_target']}-factory.bin"
        # Reject any path separators / traversal before touching disk.
        if fac_name != Path(fac_name).name or fac_name.startswith("."):
            raise SystemExit(f"{p}: factory name contains path characters: {fac_name!r}")
        if fac_name != expected_name:
            raise SystemExit(f"{p}: factory name {fac_name!r} != expected {expected_name!r}")
        # flash_size + factory_bytes come out of build_release.py and
        # must line up with the on-disk artifact.
        if b.get("flash_size") != "4MB":
            raise SystemExit(f"{p}: flash_size {b.get('flash_size')!r} != '4MB'")
        if b.get("factory_bytes") != expected_factory_size:
            raise SystemExit(
                f"{p}: factory_bytes {b.get('factory_bytes')} != expected {expected_factory_size}")
        expected_sha = b["factory"]["sha256"]
        fac_path = bundle / fac_name
        sidecar_path = bundle / f"{fac_name}.sha256"
        if not fac_path.is_file():
            raise SystemExit(f"missing factory: {fac_path}")
        if not sidecar_path.is_file():
            raise SystemExit(f"missing sidecar (mandatory): {sidecar_path}")
        actual_sha = _sha256(fac_path)
        if actual_sha != expected_sha:
            raise SystemExit(
                f"sha256 mismatch on {fac_name}: manifest {expected_sha} vs file {actual_sha}")
        expected_sidecar = f"{expected_sha}  {fac_name}\n"
        if sidecar_path.read_text() != expected_sidecar:
            raise SystemExit(
                f"sidecar {sidecar_path.name} content must equal "
                f'"{expected_sha}  {fac_name}\\n" exactly')
        actual_size = fac_path.stat().st_size
        if actual_size != expected_factory_size:
            raise SystemExit(
                f"{fac_name}: size {actual_size} != required {expected_factory_size}")
    return m


def _profile_manifest(profile: str, chip: str, fac_name: str, release_tag: str) -> dict:
    """ESP Web Tools manifest served from manifests/<profile>.json.

    Only `atoms3lite-unit-nfc` carries `serialType: cdc` -- generic
    profiles omit `serialType` so ESP Web Tools' matcher falls back to
    chip-family-only. All parts paths are relative to the manifest
    location (manifests/), so they point into ../firmware/. `version`
    is the release tag so ESP Web Tools shows the exact deployed tag.
    """
    build = {
        "chipFamily": chip,
        "improv": True,
        "parts": [{"path": f"../firmware/{fac_name}", "offset": 0}],
    }
    if profile == "atoms3lite-unit-nfc":
        build["serialType"] = "cdc"
    return {
        "name": f"HomeKey-ESP32 ({REQUIRED_PROFILES[profile]['display_name']})",
        "version": release_tag,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 30,
        "builds": [build],
    }


def _emit_into(staging: Path, bundle: Path, bm: dict, release_tag: str) -> None:
    """Populate `staging` with the required layout."""
    (staging / "firmware").mkdir(parents=True)
    (staging / "manifests").mkdir()

    for b in bm["builds"]:
        profile = b["profile"]
        chip = b["chip_family"]
        fac_name = b["factory"]["name"]
        # Copy factory + sidecar into firmware/.
        shutil.copy2(bundle / fac_name, staging / "firmware" / fac_name)
        shutil.copy2(bundle / f"{fac_name}.sha256",
                     staging / "firmware" / f"{fac_name}.sha256")
        # Per-profile ESP Web Tools manifest under manifests/.
        pm = _profile_manifest(profile, chip, fac_name, release_tag)
        (staging / "manifests" / f"{profile}.json").write_text(
            json.dumps(pm, indent=2) + "\n")

    # Static index (copied verbatim, no templating).
    shutil.copy2(SITE_TEMPLATE_DIR / "index.html", staging / "index.html")
    (staging / ".nojekyll").write_text("")

    # Provenance shipped with the site: verbatim copy of the bundle
    # manifest so host-test records and per-profile detail are preserved
    # end-to-end with no second schema to drift from.
    shutil.copy2(bundle / "manifest.json", staging / "build-manifest.json")


def generate(bundle: Path, out: Path, release_tag: str,
             expected_factory_size: int = DEFAULT_FACTORY_SIZE) -> dict:
    if out.exists():
        raise SystemExit(f"{out} already exists; refusing to overwrite")
    bm = _load_and_validate_bundle(bundle, release_tag, expected_factory_size)

    # Build in a sibling temp dir, then os.rename onto `out` so a
    # partial site is never observable.
    out.parent.mkdir(parents=True, exist_ok=True)
    staging_parent = tempfile.mkdtemp(prefix=".genpoc-", dir=str(out.parent))
    staging = Path(staging_parent) / out.name
    try:
        staging.mkdir()
        _emit_into(staging, bundle, bm, release_tag)
        os.rename(staging, out)
    except BaseException:
        shutil.rmtree(staging_parent, ignore_errors=True)
        raise
    else:
        # Clean the (now empty) parent temp dir.
        shutil.rmtree(staging_parent, ignore_errors=True)
    return {"profiles": len(bm["builds"]), "out": str(out)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bundle", type=Path, required=True,
                    help="path to a build_release.py bundle (contains manifest.json)")
    ap.add_argument("--out", type=Path, required=True,
                    help="output site directory; must NOT already exist")
    ap.add_argument("--release-tag", required=True,
                    help="release tag to pin against, must be "
                         "multi-board-poc-<bundle short commit>")
    args = ap.parse_args()
    result = generate(args.bundle, args.out, args.release_tag)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
