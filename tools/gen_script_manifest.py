#!/usr/bin/env python3
"""gen_script_manifest.py - generate and verify scripts/manifest.json.

The manifest is the single source of truth for official scripts and shaders
downloaded in-game by URUSHI.

Usage:
    python tools/gen_script_manifest.py          # regenerate scripts/manifest.json
    python tools/gen_script_manifest.py --check  # fail if scripts/manifest.json is stale or missing (for CI)
"""
import argparse
import hashlib
import json
import os
import re
import sys

SCHEMA_VERSION = 1

# Metadata for official scripts.
# default_enabled: safe scripts default to True; memory patchers / gameplay-altering mods default to False.
SCRIPTS_METADATA = {
    "telemetry_hud": {
        "name": "Telemetry HUD",
        "name_ru": "Телеметрия HUD",
        "description": "Compact on-screen HUD: speed, drift angle, RPM, FPS & ping",
        "description_ru": "Наэкранный оверлей: скорость, угол заноса, RPM, FPS и сеть",
        "window_ids": ["hud", "fps_net"],
        "min_core": "1.2.1",
        "default_enabled": True,
    },
    "perf_optimizer": {
        "name": "FPS Optimizer",
        "name_ru": "Оптимизатор FPS",
        "description": "Physics solver optimization, component auto-sleep, FPS boost",
        "description_ru": "Оптимизация физики симулятора, автослип узлов, буст FPS",
        "window_ids": ["perf"],
        "min_core": "1.2.1",
        "default_enabled": True,
    },
    "carfx_mod": {
        "name": "CarFX Shaders",
        "name_ru": "CarFX Шейдеры",
        "description": "Custom body shaders, glass reflections, neon glow & tinting",
        "description_ru": "Кастомные шейдеры кузова, стекол, неоновые эффекты, тонировка",
        "window_ids": ["carfx"],
        "min_core": "1.2.1",
        "default_enabled": True,
    },
    "keybinds_mod": {
        "name": "Keybinds & Controls",
        "name_ru": "Горячие клавиши",
        "description": "Rebind actions to keyboard keys and gamepad buttons (XInput)",
        "description_ru": "Назначение кнопок клавиатуры и кнопок геймпада (XInput)",
        "window_ids": ["keybinds"],
        "min_core": "1.2.1",
        "default_enabled": True,
    },
    "teleport_mod": {
        "name": "Checkpoint Teleport",
        "name_ru": "Телепорт по точкам",
        "description": "Quick save & instant teleport to checkpoints on any track",
        "description_ru": "Быстрое сохранение и мгновенный телепорт по точкам трека",
        "window_ids": ["teleport"],
        "min_core": "1.2.1",
        "default_enabled": True,
    },
    "replay_mod": {
        "name": "Replays & Anti-Collision",
        "name_ru": "Реплеи и Анти-встречка",
        "description": "3D trajectory replay, ghost car and TTC anti-collision defense",
        "description_ru": "3D реплеи, призрак траектории и защита от столкновений",
        "window_ids": ["replay", "anticollision"],
        "min_core": "1.2.1",
        "default_enabled": True,
    },
    "steer_assist_mod": {
        "name": "Steering Assist (155°)",
        "name_ru": "Помощь руля (155°)",
        "description": "Smart wheel angle assist for deep drift angles up to 155°",
        "description_ru": "Умная коррекция выворота колес под угол заноса до 155°",
        "window_ids": ["steer_assist"],
        "min_core": "1.2.1",
        "default_enabled": False,
    },
    "tires_mod": {
        "name": "Body & Tires",
        "name_ru": "Кузов и шины",
        "description": "Dense tire smoke, infinite tire wear, pressure and visual damage",
        "description_ru": "Густой дым, блокировка износа шин, давление и деформация",
        "window_ids": ["tires"],
        "min_core": "1.2.1",
        "default_enabled": False,
    },
}


def read_core_version(root_dir: str) -> str:
    version_txt = os.path.join(root_dir, "dist", "version.txt")
    if os.path.isfile(version_txt):
        with open(version_txt, "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("version="):
                    return line.split("=", 1)[1].strip()

    updater_h = os.path.join(root_dir, "core", "updater.h")
    if os.path.isfile(updater_h):
        with open(updater_h, "r", encoding="utf-8") as f:
            m = re.search(r'#define\s+URUSHI_VERSION\s+"([^"]+)"', f.read())
            if m:
                return m.group(1)
    return "1.2.1"


def hash_lf(file_path: str):
    with open(file_path, "rb") as f:
        data = f.read()
    normalized = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    sha256 = hashlib.sha256(normalized).hexdigest()
    return sha256, len(normalized)


def build_manifest(root_dir: str):
    core_ver = read_core_version(root_dir)

    scripts_list = []
    scripts_dir = os.path.join(root_dir, "scripts")
    for script_id in sorted(SCRIPTS_METADATA.keys()):
        meta = SCRIPTS_METADATA[script_id]
        rel_path = f"scripts/{script_id}.lua"
        abs_path = os.path.join(scripts_dir, f"{script_id}.lua")
        if not os.path.isfile(abs_path):
            raise FileNotFoundError(f"Official script file missing: {abs_path}")

        sha256, size = hash_lf(abs_path)
        scripts_list.append({
            "id": script_id,
            "path": rel_path,
            "sha256": sha256,
            "size": size,
            "name": meta["name"],
            "name_ru": meta["name_ru"],
            "description": meta["description"],
            "description_ru": meta["description_ru"],
            "window_ids": meta["window_ids"],
            "min_core": meta["min_core"],
            "default_enabled": meta["default_enabled"],
        })

    shaders_list = []
    shaders_dir = os.path.join(root_dir, "shaders")
    if os.path.isdir(shaders_dir):
        for name in sorted(os.listdir(shaders_dir)):
            if name.endswith(".hlsl") or name.endswith(".hlsli"):
                abs_path = os.path.join(shaders_dir, name)
                if os.path.isfile(abs_path):
                    sha256, size = hash_lf(abs_path)
                    shaders_list.append({
                        "path": f"shaders/{name}",
                        "sha256": sha256,
                        "size": size,
                    })

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "core_version": core_ver,
        "scripts": scripts_list,
        "shaders": shaders_list,
    }
    return manifest


def main():
    parser = argparse.ArgumentParser(description="Generate and verify scripts/manifest.json")
    parser.add_argument("--check", action="store_true", help="Fail if manifest.json is stale or missing")
    parser.add_argument("--output", type=str, default=None, help="Output file path (default: scripts/manifest.json)")
    args = parser.parse_args()

    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target_path = args.output or os.path.join(root_dir, "scripts", "manifest.json")

    manifest = build_manifest(root_dir)
    manifest_json = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"

    if args.check:
        if not os.path.isfile(target_path):
            print(f"ERROR: Manifest file missing: {target_path}", file=sys.stderr)
            sys.exit(1)
        with open(target_path, "r", encoding="utf-8") as f:
            existing_content = f.read()

        try:
            existing_manifest = json.loads(existing_content)
        except json.JSONDecodeError as e:
            print(f"ERROR: Manifest file is invalid JSON: {e}", file=sys.stderr)
            sys.exit(1)

        if existing_manifest != manifest:
            print("ERROR: scripts/manifest.json is out of date! Run tools/gen_script_manifest.py to regenerate.", file=sys.stderr)
            sys.exit(1)

        print("OK: scripts/manifest.json is up to date.")
        sys.exit(0)

    with open(target_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest_json)

    print(f"Generated {target_path} successfully ({len(manifest['scripts'])} scripts, {len(manifest['shaders'])} shaders).")


if __name__ == "__main__":
    main()
