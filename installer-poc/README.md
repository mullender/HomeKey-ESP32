# installer-poc

Local, single-machine POC for building the four active release profiles
listed in `profiles.json`:

- `esp32-generic`
- `esp32s3-generic`
- `esp32c3-generic`
- `atoms3lite-unit-nfc` (S3 + layered `sdkconfig.defaults.installer.atoms3`)

`esp32c6-generic` is a follow-up, not in the active profile set. Measured
here: the current C6 app is `0x1d8270` bytes and the OTA slot in the
project's `with_ota.csv` is `0x1d0000` (overflow ~33 KB). The POC does
not modify the partition map; addressing C6 is out of scope.

Nothing here uploads, tags, releases, or updates Pages. Outputs stay
under `dist/<short-commit>/` in the working tree (`dist/` is gitignored).

## Requirements

- Clean `git status` (tracked files) and clean submodules. The script
  refuses to run otherwise, and refuses to emit a bundle if a build
  changes tracked files. Each isolated build gets its own
  `dependencies.lock` (redirected via the `HOMEKEY_DEPENDENCIES_LOCK`
  opt-in in the root `CMakeLists.txt`), so the tracked repo-root
  `dependencies.lock` is left untouched. As a guard, the script fails
  hard if the root lock's sha256 changes at any point during the run.
- ESP-IDF exported (`. $IDF_PATH/export.sh`) so `idf.py` is on `PATH`.
  `idf.py --version` must succeed; the script fails hard otherwise.
- `bun` on `PATH`. `main/CMakeLists.txt` invokes `bun install` and
  `bun run build` to produce the web assets baked into spiffs; `bun
  --version` must succeed and is recorded in the manifest.
- Python 3 with just the standard library.

## Usage

```
$ cd installer-poc
$ ./build_release.py            # build all four profiles
$ ./build_release.py --plan     # print the validated plan, no builds, no files
$ ./build_release.py --only esp32s3-generic atoms3lite-unit-nfc
```

## What each build does

For every profile:

1. Delete `build.poc/<profile>` if it exists (stale-output guard).
2. `idf.py -B build.poc/<profile> -DIDF_TARGET=<idf_target>
   -DSDKCONFIG_DEFAULTS=<;-joined defaults> build`.
3. Assert `dependencies.lock` did not change (if it did, the run fails
   without restoring the lock).
4. `idf.py -B build.poc/<profile> ... merge-bin`. The merged image
   comes out of IDF's own pipeline at `build.poc/<profile>/merged-binary.bin`
   -- no direct `esptool` invocation, no flash_args parsing.
5. Verify `config/sdkconfig.json` has `IDF_TARGET == profile.idf_target`,
   that `INSTALLER_ATOMS3_LITE_DEFAULTS` matches the profile's declared
   `installer_flag` (must be `true` for `atoms3lite-unit-nfc` and `false`
   for every generic profile), that the app `.bin` is at least 100 KB,
   and that `merged-binary.bin` is exactly 4 MiB (4194304 bytes).
6. Copy app + factory into `dist/<short-commit>/`, write per-file
   `.sha256` sidecars, and append a per-profile entry to
   `dist/<short-commit>/manifest.json`.

## Output layout

```
dist/
  <short-commit>/
    HomeKey-ESP32-<short>-esp32-generic-esp32-app.bin
    HomeKey-ESP32-<short>-esp32-generic-esp32-app.bin.sha256
    HomeKey-ESP32-<short>-esp32-generic-esp32-factory.bin
    HomeKey-ESP32-<short>-esp32-generic-esp32-factory.bin.sha256
    ... one quad per profile ...
    manifest.json          # commit, branch, IDF version, submodule SHAs,
                           # host-suite pass records, per-profile size + sha256
```

## `--plan`

Runs preflight and provenance, then prints the validated plan as JSON to
stdout. Creates no files, no directories. No host tests, no `idf.py`,
no artifacts.

## Non-goals

- No CI integration.
- No uploads. No GitHub release. No Pages workflow.
- The script does not edit tracked source. IDF and Bun write
  generated, ignored files under `build.poc` and `data`.
- No parallelism -- single serial pass, easy to reason about failures.
