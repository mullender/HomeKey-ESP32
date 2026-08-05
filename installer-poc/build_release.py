#!/usr/bin/env python3
"""Local release-build POC. See installer-poc/README.md for scope + usage."""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
POC_DIR = Path(__file__).resolve().parent
PROFILES_PATH = POC_DIR / "profiles.json"
BUILD_ROOT_NAME = "build.poc"
DIST_ROOT_NAME = "dist"


def run_captured(cmd, cwd=None):
    """Run cmd (list) capturing stdout; check=True. Returns CompletedProcess."""
    return subprocess.run(cmd, cwd=cwd or REPO, check=True,
                          capture_output=True, text=True)


def run_streamed(cmd, cwd=None):
    """Run cmd streaming stdout/stderr live; check=True."""
    subprocess.run(cmd, cwd=cwd or REPO, check=True)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def preflight_clean_tree():
    """Refuse to build from a dirty tree or dirty submodules (recursive)."""
    porcelain = run_captured(["git", "status", "--porcelain"]).stdout
    if porcelain.strip():
        raise SystemExit(f"tree is dirty; refusing to build:\n{porcelain}")
    subs = run_captured(["git", "submodule", "status", "--recursive"]).stdout
    for line in subs.splitlines():
        if line[:1] in "+-U":
            raise SystemExit(f"submodule dirty; refusing to build:\n{subs}")


def dependencies_lock_hash():
    p = REPO / "dependencies.lock"
    return sha256_of(p) if p.exists() else None


def assert_lock_unchanged(baseline, where):
    """Every idf.py invocation mutates dependencies.lock. The POC must not
    silently rewrite source; if a build changes the lock, fail loudly and
    let the user decide whether the underlying dependency change is real."""
    now = dependencies_lock_hash()
    if now != baseline:
        raise SystemExit(
            f"dependencies.lock changed {where}. Not restoring silently; "
            f"inspect the diff and commit intentionally if the change is real.")


def collect_provenance():
    """Fail hard if idf.py is missing -- the whole POC depends on it."""
    head = run_captured(["git", "rev-parse", "HEAD"]).stdout.strip()
    short = run_captured(["git", "rev-parse", "--short", "HEAD"]).stdout.strip()
    branch = run_captured(["git", "rev-parse", "--abbrev-ref", "HEAD"]).stdout.strip()
    subs = []
    for line in run_captured(["git", "submodule", "status", "--recursive"]).stdout.splitlines():
        parts = line.strip().split(None, 2)
        if len(parts) >= 2:
            subs.append({"commit": parts[0].lstrip("+-U "), "path": parts[1]})
    idf_ver = run_captured(["idf.py", "--version"]).stdout.strip().splitlines()[0]
    # main/CMakeLists.txt invokes `bun install`/`bun run build` to
    # produce the web assets baked into spiffs. Record the version so a
    # mystery output change can be traced to a bun bump.
    bun_ver = run_captured(["bun", "--version"]).stdout.strip()
    return {
        "commit": head,
        "commit_short": short,
        "branch": branch,
        "built_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "idf_version": idf_ver,
        "bun_version": bun_ver,
        "submodules": subs,
    }


def run_host_tests():
    """Stream each suite; record path + status only. No number-parsing."""
    suites = [
        REPO / "main" / "test" / "run_tests.sh",
        REPO / "components" / "improv" / "test" / "run_tests.sh",
    ]
    results = []
    for suite in suites:
        print(f"==> host tests: {suite.relative_to(REPO)}", flush=True)
        rc = subprocess.run(["bash", str(suite)], cwd=REPO).returncode
        if rc != 0:
            raise SystemExit(f"host tests failed: {suite}")
        results.append({"suite": str(suite.relative_to(REPO)), "status": "passed"})
    return results


def read_sdkconfig_json(build_dir: Path) -> dict:
    return json.loads((build_dir / "config" / "sdkconfig.json").read_text())


def build_profile(profile, lock_baseline):
    """idf.py build + idf.py merge-bin for one profile. Returns a dict."""
    build_dir = REPO / BUILD_ROOT_NAME / profile["name"]
    if build_dir.exists():
        shutil.rmtree(build_dir)   # prevent stale outputs across POC runs
    build_dir.parent.mkdir(exist_ok=True)

    defaults_arg = "-DSDKCONFIG_DEFAULTS=" + ";".join(profile["sdkconfig_defaults"])
    target_arg = f"-DIDF_TARGET={profile['idf_target']}"
    # Per-build lock keeps IDF's component manager from writing back to
    # the tracked repo-root dependencies.lock (which every target
    # resolution would otherwise mutate). Consumed by the opt-in in
    # the root CMakeLists.txt (HOMEKEY_DEPENDENCIES_LOCK).
    lock_arg = f"-DHOMEKEY_DEPENDENCIES_LOCK={build_dir / 'dependencies.lock'}"
    # Per-build sdkconfig too: otherwise IDF writes it at the project
    # root and the next profile trips the "existing sdkconfig target
    # doesn't match IDF_TARGET" guard from targets.cmake.
    sdkconfig_arg = f"-DSDKCONFIG={build_dir / 'sdkconfig'}"
    common = ["idf.py", "-B", str(build_dir),
              target_arg, defaults_arg, lock_arg, sdkconfig_arg]

    print(f"  [{profile['name']}] {' '.join(common + ['build'])}", flush=True)
    run_streamed(common + ["build"])
    assert_lock_unchanged(lock_baseline, f"after {profile['name']} build")

    print(f"  [{profile['name']}] {' '.join(common + ['merge-bin'])}", flush=True)
    run_streamed(common + ["merge-bin"])
    assert_lock_unchanged(lock_baseline, f"after {profile['name']} merge-bin")

    # Verify what the build produced matches the profile.
    sdk = read_sdkconfig_json(build_dir)
    if sdk.get("IDF_TARGET") != profile["idf_target"]:
        raise SystemExit(f"{profile['name']}: IDF_TARGET mismatch: "
                         f"expected {profile['idf_target']}, got {sdk.get('IDF_TARGET')}")
    got_flag = bool(sdk.get("INSTALLER_ATOMS3_LITE_DEFAULTS", False))
    if got_flag != profile["installer_flag"]:
        raise SystemExit(f"{profile['name']}: INSTALLER_ATOMS3_LITE_DEFAULTS mismatch: "
                         f"expected {profile['installer_flag']}, got {got_flag}")

    # Every expected artifact must exist; every factory image must be
    # exactly 4 MiB. No wiggle room in the POC.
    app_bin = build_dir / "HomeKey-ESP32.bin"
    factory_bin = build_dir / "merged-binary.bin"
    for expected in (app_bin, factory_bin):
        if not expected.is_file():
            raise SystemExit(f"{profile['name']}: missing expected artifact {expected}")
    app_size = app_bin.stat().st_size
    factory_size = factory_bin.stat().st_size
    if app_size < 100 * 1024:
        raise SystemExit(f"{profile['name']}: app bin implausibly small: {app_size} bytes")
    if factory_size != 4 * 1024 * 1024:
        raise SystemExit(f"{profile['name']}: factory image {factory_size} bytes; "
                         f"POC requires exactly 4 MiB (4194304 bytes)")

    return {
        "sdkconfig_defaults": profile["sdkconfig_defaults"],
        "flash_size": sdk.get("ESPTOOLPY_FLASHSIZE", "4MB"),
        "app_bin": app_bin,
        "app_bytes": app_size,
        "app_sha256": sha256_of(app_bin),
        "factory_bin": factory_bin,
        "factory_bytes": factory_size,
        "factory_sha256": sha256_of(factory_bin),
    }


def emit_artifacts(profile, build, short, dist_dir):
    """Copy app + factory to dist and write .sha256 sidecars.
    Returns the manifest builds[] entry."""
    dist_dir.mkdir(parents=True, exist_ok=True)
    stem = f"HomeKey-ESP32-{short}-{profile['name']}-{profile['idf_target']}"
    out_app = dist_dir / f"{stem}-app.bin"
    out_fac = dist_dir / f"{stem}-factory.bin"
    shutil.copy2(build["app_bin"], out_app)
    shutil.copy2(build["factory_bin"], out_fac)
    (dist_dir / f"{out_app.name}.sha256").write_text(f"{build['app_sha256']}  {out_app.name}\n")
    (dist_dir / f"{out_fac.name}.sha256").write_text(f"{build['factory_sha256']}  {out_fac.name}\n")
    return {
        "profile": profile["name"],
        "chip_family": profile["chip_family"],
        "idf_target": profile["idf_target"],
        "sdkconfig_defaults": build["sdkconfig_defaults"],
        "installer_flag": profile["installer_flag"],
        "flash_size": build["flash_size"],
        "app_bytes": build["app_bytes"],
        "factory_bytes": build["factory_bytes"],
        "app":     {"name": out_app.name, "sha256": build["app_sha256"]},
        "factory": {"name": out_fac.name, "sha256": build["factory_sha256"]},
    }


def load_profiles(only):
    doc = json.loads(PROFILES_PATH.read_text())
    if doc.get("schema_version") != 1:
        raise SystemExit("profiles.json: unsupported schema_version")
    profiles = doc["profiles"]
    if only:
        wanted = set(only)
        profiles = [p for p in profiles if p["name"] in wanted]
        missing = wanted - {p["name"] for p in profiles}
        if missing:
            raise SystemExit(f"unknown profile(s): {sorted(missing)}")
    return profiles


def main():
    ap = argparse.ArgumentParser(description="Local release-build POC")
    ap.add_argument("--plan", action="store_true",
                    help="print the validated plan only; no files, no builds, no host tests")
    ap.add_argument("--only", nargs="+", metavar="PROFILE",
                    help="restrict to specific profile names")
    ap.add_argument("--out", type=Path, default=REPO / DIST_ROOT_NAME,
                    help=f"output root (default: {DIST_ROOT_NAME}/)")
    args = ap.parse_args()

    profiles = load_profiles(args.only)
    preflight_clean_tree()
    prov = collect_provenance()

    if args.plan:
        plan = {
            "plan": True,
            **prov,
            "profiles": [{
                "name": p["name"],
                "chip_family": p["chip_family"],
                "idf_target": p["idf_target"],
                "sdkconfig_defaults": p["sdkconfig_defaults"],
                "installer_flag": p["installer_flag"],
            } for p in profiles],
        }
        print(json.dumps(plan, indent=2))
        return 0

    # Refuse to overwrite an existing bundle for this commit. Old bundles
    # must be removed explicitly so provenance never gets clobbered.
    dist_dir = args.out / prov["commit_short"]
    if dist_dir.exists():
        raise SystemExit(f"{dist_dir} already exists; remove it and re-run.")

    lock_baseline = dependencies_lock_hash()
    host_tests = run_host_tests()
    assert_lock_unchanged(lock_baseline, "after host tests")

    # Collect build results before writing output. Binaries stay in each
    # build.poc/<profile>/ dir on disk; dist/<short>/ is only created
    # once every selected profile has succeeded, so a partial run never
    # leaves a half-populated bundle behind.
    results = []
    for profile in profiles:
        print(f"==> {profile['name']}", flush=True)
        build = build_profile(profile, lock_baseline)
        results.append((profile, build))

    assert_lock_unchanged(lock_baseline, "before emitting bundle")
    # Catch tracked changes from build tools before bundle emission.
    preflight_clean_tree()

    manifest = {
        "schema_version": 1,
        "plan": False,
        **prov,
        "host_tests": host_tests,
        "builds": [],
    }
    dist_dir.mkdir(parents=True)
    for profile, build in results:
        manifest["builds"].append(emit_artifacts(profile, build, prov["commit_short"], dist_dir))

    mpath = dist_dir / "manifest.json"
    mpath.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"manifest written: {mpath}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
