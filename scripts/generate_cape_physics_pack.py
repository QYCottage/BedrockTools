#!/usr/bin/env python3
"""Build the BedrockTools Cape Physics resource-pack sources.

The pack models a cape as 24 narrow, parented cloth strips.  Its animation is
kept separate from the native module: Minecraft evaluates the Molang queries,
which makes movement client-side and server-independent.

This script intentionally generates the repetitive bone declarations instead
of keeping a hand-edited, error-prone 24-bone JSON file in the repository.
"""

from __future__ import annotations

import argparse
import json
import shutil
import uuid
import zipfile
from pathlib import Path

SEGMENTS = 24
TEXTURE_WIDTH = 64
TEXTURE_HEIGHT = 32


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def cape_animation() -> dict:
    bones: dict[str, dict] = {
        "cape": {
            "rotation": [
                "math.clamp(math.lerp(0, -110, q.cape_flap_amount) - (13 * q.modified_move_speed) - ((1.0 - q.is_on_ground) * 10.0), -80, 10)",
                "q.modified_move_speed * math.pow(math.sin(q.body_y_rotation - q.head_y_rotation(0)), 3.1) * 46",
                "0",
            ],
            "position": ["0", "0.4", "2.6"],
        }
    }
    for index in range(1, SEGMENTS + 1):
        phase = index * 8
        walk = 2.5 + index * 0.0652173913
        sprint = 1.0 + index * 0.0434782609
        jump = 1.5 + index * 0.0652173913
        air = 1.0 + index * 0.0347826087
        idle = 0.8 + index * 0.0304347826
        turn = 2.0 + index * 0.2
        bones[f"bone_{index}"] = {
            "rotation": [
                "math.clamp("
                f"-q.modified_move_speed * math.sin(q.modified_distance_moved * 16.0 + {phase}) * {walk:.3f} "
                f"+ (q.is_sprinting ? -q.modified_move_speed * math.sin(q.modified_distance_moved * 22.0 + {phase}) * {sprint:.3f} : 0.0) "
                f"+ (q.is_jumping ? math.sin(q.life_time * 90.0 + {phase}) * {jump:.3f} * -1.0 : 0.0) "
                f"- ((1.0 - q.is_on_ground) * (1.0 - q.is_jumping) * {air:.3f}) "
                f"+ math.sin(q.life_time * 105.0 + {phase}) * {idle:.3f} "
                "* (1.0 - math.min(q.modified_move_speed * 1.8, 1.0)), -40, 20)",
                "0",
                f"q.modified_move_speed * math.sin(q.body_y_rotation - q.head_y_rotation(0)) * {turn:.1f}",
            ]
        }
    return {
        "format_version": "1.8.0",
        "animations": {"animation.player.cape": {"loop": True, "bones": bones}},
    }


def cape_geometry() -> dict:
    # A 10 x 16 classic cape is divided into 24 strips.  Every strip is a
    # child of its predecessor so the animation produces a smooth chain.
    strip_height = 16.0 / SEGMENTS
    bones: list[dict] = [{"name": "cape", "pivot": [0, 24, 2]}]
    for index in range(1, SEGMENTS + 1):
        bone: dict = {
            "name": f"bone_{index}",
            "parent": "cape" if index == 1 else f"bone_{index - 1}",
            "pivot": [0, 24 - (index - 1) * strip_height, 2],
            "cubes": [
                {
                    "origin": [-5, 24 - index * strip_height, 1.5],
                    "size": [10, strip_height + 0.03, 1],
                    "uv": [1, 1 + (index - 1) * strip_height],
                }
            ],
        }
        bones.append(bone)
    return {
        "format_version": "1.12.0",
        "minecraft:geometry": [
            {
                "description": {
                    # minecraft:player maps its cape slot to geometry.cape.
                    # Reusing that identifier overrides only the vanilla cape
                    # mesh; the player entity and its render controllers stay
                    # untouched, which keeps this safe across 1.26.x patches.
                    "identifier": "geometry.cape",
                    "texture_width": TEXTURE_WIDTH,
                    "texture_height": TEXTURE_HEIGHT,
                    "visible_bounds_width": 3,
                    "visible_bounds_height": 3,
                    "visible_bounds_offset": [0, 1.5, 0],
                },
                "bones": bones,
            }
        ],
    }


def manifest() -> dict:
    return {
        "format_version": 2,
        "header": {
            "name": "BedrockTools Cape Physics",
            "description": "Client-side segmented cape animation for BedrockTools.",
            "uuid": "c251bb22-0592-48fd-8e6a-3e56c1322e1a",
            "version": [1, 0, 0],
            "min_engine_version": [1, 26, 40],
        },
        "modules": [
            {
                "type": "resources",
                "uuid": "de2085a6-a6ea-4972-badc-f1d211c064ce",
                "version": [1, 0, 0],
            }
        ],
    }


def build(destination: Path) -> Path:
    root = destination / "BedrockTools Cape Physics"
    if root.exists():
        shutil.rmtree(root)
    write_json(root / "manifest.json", manifest())
    write_json(root / "animations" / "Cape.animation.json", cape_animation())
    write_json(root / "models" / "entity" / "Cape.geo.json", cape_geometry())
    (root / "README.txt").write_text(
        "Cape Physics resource pack for Minecraft Bedrock 1.26.x.\n"
        "Enable this pack in Global Resources and place it above packs that replace the player or cape model.\n"
        "It overrides the vanilla geometry.cape and animation.player.cape identifiers only.\n", 
        encoding="utf-8",
    )
    return root


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("out/cape-physics"))
    parser.add_argument("--mcpack", type=Path)
    args = parser.parse_args()
    root = build(args.output)
    if args.mcpack:
        args.mcpack.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.mcpack, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for source in root.rglob("*"):
                if source.is_file():
                    archive.write(source, source.relative_to(root))
        print(args.mcpack)
    else:
        print(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
