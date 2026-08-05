#!/usr/bin/env python3
"""Focused tests for installer-poc/generate_site.py. Run with:
    python3 -m unittest installer-poc.test_generate_site
"""

import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_site as G  # noqa: E402


TEST_FACTORY_SIZE = 4096   # small fixture size, overrides the 4-MiB check
TEST_SHORT = "abc1234"
TEST_FULL = TEST_SHORT + "0" * (40 - len(TEST_SHORT))
TEST_TAG = f"multi-board-poc-{TEST_SHORT}"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _factory_bytes(profile: str) -> bytes:
    """Deterministic factory payload per profile so hashes differ."""
    seed = profile.encode() + b"\x00"
    reps = TEST_FACTORY_SIZE // len(seed) + 1
    return (seed * reps)[:TEST_FACTORY_SIZE]


def _expected_factory_name(profile: str, short: str) -> str:
    target = G.REQUIRED_PROFILES[profile]["idf_target"]
    return f"HomeKey-ESP32-{short}-{profile}-{target}-factory.bin"


def _make_bundle(bundle: Path, *,
                 commit_short=TEST_SHORT,
                 commit_full=TEST_FULL,
                 include_all=True,
                 extra_profiles=None,
                 duplicate_profile=None,
                 corrupt_sidecar_of=None,
                 corrupt_factory_of=None,
                 chip_override=None,
                 flag_override=None,
                 target_override=None,
                 name_override=None,
                 flash_size="4MB",
                 factory_bytes=TEST_FACTORY_SIZE) -> str:
    """Build a fake bundle. Returns the expected release tag."""
    bundle.mkdir(parents=True)
    order = list(G.REQUIRED_PROFILES.keys())
    if not include_all:
        order.remove("atoms3lite-unit-nfc")
    if extra_profiles:
        order = order + list(extra_profiles)
    if duplicate_profile:
        order = order + [duplicate_profile]

    builds = []
    for p in order:
        contract = G.REQUIRED_PROFILES.get(p, {
            "chip_family": "ESP32-S3", "idf_target": "esp32s3", "installer_flag": False})
        chip = chip_override if (chip_override and p == "esp32s3-generic") else contract["chip_family"]
        target = target_override if (target_override and p == "esp32s3-generic") else contract["idf_target"]
        flag = flag_override if (flag_override is not None and p == "atoms3lite-unit-nfc") else contract["installer_flag"]

        default_name = f"HomeKey-ESP32-{commit_short}-{p}-{target}-factory.bin"
        fac_name = name_override if (name_override and p == "esp32-generic") else default_name

        data = _factory_bytes(p)
        (bundle / fac_name).write_bytes(data)
        real_sha = _sha(data)
        stored_sha = real_sha
        if corrupt_factory_of == p:
            (bundle / fac_name).write_bytes(b"\xFF" * TEST_FACTORY_SIZE)
        sidecar_text = f"{stored_sha}  {fac_name}\n"
        if corrupt_sidecar_of == p:
            sidecar_text = f"{stored_sha} {fac_name}\n"   # single-space, wrong
        (bundle / f"{fac_name}.sha256").write_text(sidecar_text)
        builds.append({
            "profile": p,
            "chip_family": chip,
            "idf_target": target,
            "sdkconfig_defaults": ["sdkconfig.defaults"],
            "installer_flag": flag,
            "flash_size": flash_size,
            "app_bytes": 1024,
            "factory_bytes": factory_bytes,
            "app": {"name": f"{fac_name}.dummy-app", "sha256": _sha(b"app")},
            "factory": {"name": fac_name, "sha256": stored_sha},
        })
    manifest = {
        "schema_version": 1,
        "plan": False,
        "commit": commit_full,
        "commit_short": commit_short,
        "branch": "test",
        "built_at_utc": "1970-01-01T00:00:00Z",
        "idf_version": "test",
        "bun_version": "test",
        "submodules": [{"commit": "0"*40, "path": "components/x"}],
        "host_tests": [{"suite": "main/test/run_tests.sh", "status": "passed"}],
        "builds": builds,
    }
    (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return f"multi-board-poc-{commit_short}"


class GenerateSiteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="genpoc_"))
        self.bundle = self.tmp / "bundle"
        self.out = self.tmp / "site"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # --- happy path -----------------------------------------------------

    def test_full_bundle_produces_required_layout(self):
        tag = _make_bundle(self.bundle)
        r = G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        self.assertEqual(r["profiles"], 4)
        self.assertTrue((self.out / "index.html").is_file())
        self.assertTrue((self.out / ".nojekyll").is_file())
        self.assertTrue((self.out / "build-manifest.json").is_file())
        for p in G.REQUIRED_PROFILES:
            self.assertTrue((self.out / "manifests" / f"{p}.json").is_file(), p)
        firmware = list((self.out / "firmware").iterdir())
        self.assertEqual(len(firmware), 4 * 2)
        for f in firmware:
            self.assertTrue(f.name.endswith(("-factory.bin", "-factory.bin.sha256")))
        self.assertFalse((self.out / "esp32s3-generic").exists())

    def test_manifest_shape_is_ewt_compatible(self):
        tag = _make_bundle(self.bundle)
        G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        for p, contract in G.REQUIRED_PROFILES.items():
            pm = json.loads((self.out / "manifests" / f"{p}.json").read_text())
            self.assertEqual(
                pm["name"], f"HomeKey-ESP32 ({contract['display_name']})")
            self.assertEqual(pm["new_install_prompt_erase"], True)
            self.assertEqual(pm["new_install_improv_wait_time"], 30)
            self.assertEqual(len(pm["builds"]), 1)
            b = pm["builds"][0]
            self.assertEqual(b["chipFamily"], contract["chip_family"])
            self.assertTrue(b["improv"])
            self.assertEqual(len(b["parts"]), 1)
            self.assertEqual(b["parts"][0]["offset"], 0)
            self.assertTrue(b["parts"][0]["path"].startswith("../firmware/"))

    def test_only_atoms3_carries_serial_type_cdc(self):
        tag = _make_bundle(self.bundle)
        G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        atom = json.loads((self.out / "manifests" / "atoms3lite-unit-nfc.json").read_text())
        self.assertEqual(atom["builds"][0].get("serialType"), "cdc")
        for p in ("esp32-generic", "esp32s3-generic", "esp32c3-generic"):
            pm = json.loads((self.out / "manifests" / f"{p}.json").read_text())
            self.assertNotIn("serialType", pm["builds"][0],
                             f"{p} must OMIT serialType")

    def test_profile_manifest_version_is_release_tag(self):
        tag = _make_bundle(self.bundle)
        G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        for p in G.REQUIRED_PROFILES:
            pm = json.loads((self.out / "manifests" / f"{p}.json").read_text())
            self.assertEqual(pm["version"], tag)

    def test_build_manifest_is_verbatim_copy_of_bundle_manifest(self):
        tag = _make_bundle(self.bundle)
        G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        src = (self.bundle / "manifest.json").read_bytes()
        dst = (self.out / "build-manifest.json").read_bytes()
        self.assertEqual(src, dst)
        # Verifies host_tests and bun_version are preserved.
        parsed = json.loads(dst)
        self.assertEqual(parsed["host_tests"][0]["suite"],
                         "main/test/run_tests.sh")
        self.assertEqual(parsed["bun_version"], "test")

    # --- refusal / safety ---------------------------------------------

    def test_release_tag_must_match_commit_short(self):
        _make_bundle(self.bundle)
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out,
                       "multi-board-poc-deadbee",
                       expected_factory_size=TEST_FACTORY_SIZE)

    def test_manifest_commit_must_start_with_commit_short(self):
        _make_bundle(self.bundle, commit_full="ffffffff" + "0" * 32)   # doesn't start with abc1234
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, TEST_TAG,
                       expected_factory_size=TEST_FACTORY_SIZE)

    def test_missing_required_profile_is_rejected(self):
        tag = _make_bundle(self.bundle, include_all=False)   # no atoms3
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_unexpected_profile_is_rejected(self):
        tag = _make_bundle(self.bundle, extra_profiles=["some-other"])
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_duplicate_profile_is_rejected(self):
        tag = _make_bundle(self.bundle, duplicate_profile="esp32-generic")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_chip_family_mismatch_is_rejected(self):
        tag = _make_bundle(self.bundle, chip_override="ESP32-C3")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_idf_target_mismatch_is_rejected(self):
        tag = _make_bundle(self.bundle, target_override="esp32c3")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_installer_flag_mismatch_is_rejected(self):
        tag = _make_bundle(self.bundle, flag_override=False)
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_installer_flag_must_be_actual_boolean(self):
        # A truthy non-bool (integer 1) that would sneak past bool() must
        # be rejected. Strict `is` check catches it.
        tag = _make_bundle(self.bundle, flag_override=1)
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_exact_factory_filename_is_required(self):
        # esp32-generic's factory name doesn't match the expected pattern.
        tag = _make_bundle(self.bundle,
                           name_override=f"HomeKey-ESP32-{TEST_SHORT}-esp32-generic-esp32-WRONG.bin")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_factory_filename_with_path_separator_is_rejected(self):
        tag = _make_bundle(self.bundle,
                           name_override=f"../{_expected_factory_name('esp32-generic', TEST_SHORT)}")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_missing_sidecar_is_rejected(self):
        tag = _make_bundle(self.bundle)
        # Delete one sidecar.
        for s in self.bundle.glob("*-factory.bin.sha256"):
            s.unlink()
            break
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_sidecar_content_must_be_exact(self):
        tag = _make_bundle(self.bundle, corrupt_sidecar_of="esp32-generic")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_sha_mismatch_on_disk_is_rejected(self):
        tag = _make_bundle(self.bundle, corrupt_factory_of="esp32-generic")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_flash_size_must_be_4mb(self):
        tag = _make_bundle(self.bundle, flash_size="8MB")
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_factory_bytes_field_must_match_expected_size(self):
        tag = _make_bundle(self.bundle, factory_bytes=TEST_FACTORY_SIZE + 1)
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_existing_out_is_refused(self):
        tag = _make_bundle(self.bundle)
        self.out.mkdir()
        with self.assertRaises(SystemExit):
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)

    def test_index_html_lists_each_profile_once_and_atom_is_first(self):
        tag = _make_bundle(self.bundle)
        G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        html = (self.out / "index.html").read_text()
        for p in G.REQUIRED_PROFILES:
            url = f'./manifests/{p}.json'
            self.assertEqual(html.count(url), 1,
                             f"{url} should appear exactly once")
        # AtomS3 preset row must come before every generic row.
        atom_pos = html.find('./manifests/atoms3lite-unit-nfc.json')
        self.assertGreaterEqual(atom_pos, 0)
        for gp in ("esp32-generic", "esp32s3-generic", "esp32c3-generic"):
            self.assertLess(atom_pos, html.find(f'./manifests/{gp}.json'),
                            f"AtomS3 must be listed before {gp}")
        # Semantic table structure so the mobile-CSS `thead { display:none }`
        # actually hides the unwrapped header row.
        self.assertIn("<thead>", html)
        self.assertIn("<tbody>", html)

    def test_failed_generation_leaves_no_partial_bundle(self):
        tag = _make_bundle(self.bundle, corrupt_sidecar_of="esp32-generic")
        try:
            G.generate(self.bundle, self.out, tag, expected_factory_size=TEST_FACTORY_SIZE)
        except SystemExit:
            pass
        self.assertFalse(self.out.exists())
        parent = self.out.parent
        leftovers = [p for p in parent.iterdir() if p.name.startswith(".genpoc-")]
        self.assertEqual(leftovers, [], leftovers)


if __name__ == "__main__":
    unittest.main()
