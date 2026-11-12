# URUSHI — verified scripts

This branch is the **reviewed script library** that the in-game *System → Script Library* installs from.
It contains no mod code, only `community/<id>/` script folders and the generated `catalog.json`.

```
catalog.json                generated: python tools/cloud_catalog.py
community/<id>/manifest.json
community/<id>/init.lua     entry file (default), plus any .lua/.json/.txt/.png/.hlsl files (max 32, 512 KB each)
```

## Rules
* Only the maintainer merges here. Protect the branch (require review, no force-push): the SHA-256 checks in the catalog prove the
  files match the catalog, trust comes from who can change this branch.
* After any change run `python tools/cloud_catalog.py` and commit `catalog.json` (CI runs `--check`).
* Review checklist (see `docs/SECURITY.md` on `main`): no obfuscation (`load` of built strings, `string.char` chains), no minified blobs,
  nothing that tries to reach `_G`, `getmetatable`, `debug`, builds paths with `..`, or uses `memory.*` without a gameplay reason.

## Adding a script
1. Copy the reviewed folder to `community/<id>/` (id: `a-z 0-9 _ -`, max 40) and write `manifest.json`
   (`id`, `name`, `name_ru`, `author`, `version`, `description`, `description_ru`, optional `min_urushi`, `entry`).
2. `python tools/cloud_catalog.py` → commit `community/` and `catalog.json`.
3. Bump `version` in the manifest for every update; clients show "Update" when the version differs.

Scripts run isolated: normal API, no network / process execution, `io.open("x.json")` writes to `urushi_data/<script>/`,
`config.get/set` is bound to the script's own section, `include("lib/x.lua")` loads sibling files.

Docs on `main`: `docs/CLOUD_SCRIPTS.md`, `docs/SECURITY.md`.
