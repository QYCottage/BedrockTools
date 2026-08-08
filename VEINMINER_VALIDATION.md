# Vein Miner integration validation — BedrockTools 1.5.0

This revision was rebuilt from the original `BedrockTools-main.zip` supplied with the matching `libminecraftpe.so`.

## Integration

- `VeinMinerModule` is included by `src/modules/ModuleRegistry.cpp`.
- `registry.emplace<VeinMinerModule>()` makes it part of the normal BedrockTools module list even while disabled.
- `xmake.lua` already compiles `src/modules/**.cpp`, so `src/modules/player/veinminer.cpp` is automatically part of `libBedrockTools.so`.
- BedrockTools version is `1.5.0`, making it easy to distinguish this build from 1.4.1/1.4.3 installs.
- GitHub Actions now runs `scripts/verify_veinminer.py` before compiling.

## Verified addresses in the supplied libminecraftpe.so

- `SurvivalMode::destroyBlock`: RVA `0xEF7129C` — signature occurs exactly once.
- `BlockSource::getBlock`: RVA `0xF24E280` — signature occurs exactly once.
- normal/local-player tick hook: RVA `0xA640524` — signature occurs exactly once.

The ARM64 code reached by `SurvivalMode::destroyBlock` reads `[x0 + 0x8]` as the player and reads the destroy-progress float at `[x19 + 0x24]`, validating `GameMode::mPlayer = 0x8` and `GameMode::mDestroyProgress = 0x24` for this binary.

## Static compilation checks

The following modified translation units passed `clang++ -std=c++20 -fsyntax-only` with host stubs for Android/Preloader-only headers:

- `src/modules/player/veinminer.cpp`
- `src/modules/ModuleRegistry.cpp`
- `src/core/memory/Signatures.cpp`

This validates C++ syntax and integration at source level. A real `arm64-v8a` Android build still has to be produced by the included GitHub Actions workflow (NDK r28c + xmake) and tested inside Minecraft/LeviLauncher; host-side checks cannot prove live server behavior.

## Vein Miner behavior

- Ores mode: mines connected ore family, including normal/deepslate variants and addon blocks ending in `_ore`.
- Logs mode: mines connected log/wood/stem/hyphae family, including stripped variants.
- Same Block mode: mines only the exact block ID.
- Max Blocks: 1–128 (default 32).
- Blocks Per Tick: 1–8 (default 1).
- Diagonal Connections: on/off.
- Queued blocks temporarily set the GameMode break progress to `1.0`, call the original `SurvivalMode::destroyBlock` trampoline, and restore the prior progress. This addresses the survival progress gate while still using Minecraft's normal destruction path instead of `/setblock`.
