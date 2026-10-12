# Aim Assist module — development notes

Target configuration: Levi Launcher 1.5.25, Minecraft Bedrock 26.51.

## Added behavior

- Adds an `Aim Assist` module to the existing BedrockTools menu.
- Adds menu toggles for player and mob targets.
- Adds sliders for target range, angular FOV, and smoothing strength.
- Uses the repository's `ActorFetchNearbyActorsSorted`, `ActorIsPlayer`, `ActorIsInvisible`, actor position/bounds, and rotation-component wrappers.
- Selects the eligible target closest to the current crosshair direction and gradually adjusts the local player's rotation component toward the target's upper body.
- Adds a small target stickiness preference to reduce rapidly swapping between similarly aligned targets.

## Default settings

- Players: on
- Mobs: on
- Range: 5.5 blocks
- Angular selection limit: 35 degrees
- Smoothing strength: 0.12 per tick

The module is off by default like the other modules and must be enabled from the LeviLauncher menu. Use it only in a private training world or server where everyone involved has agreed to it.

## Compatibility and validation status

This is a **development source build**, not a tested `.levipack` release. The new translation unit passed a strict C++ syntax check using temporary stubs for the project's JSON dependency. The project could not be fully compiled in this environment because `xmake` and Android NDK r28c are not installed here. Runtime behavior, native signature resolution, and camera rotation writes still need validation on Minecraft 26.51 before everyday use.

The implementation follows the same nearby-actor result layout, entity-category bit, and actor rotation access patterns already used in the supplied BedrockTools source. Those structures depend on version-specific native behavior; keep a backup of the world and disable this module immediately if the game crashes or the camera behaves incorrectly.

## Building

Follow the repository's Development Setup and build commands in `README.md` with Android NDK r28c, xmake, and Python 3. The normal Android release build should generate both `libBedrockTools.so` and `BedrockTools.levipack`. Do not manually rename a previous `.levipack` and assume it contains this module; the native library must be rebuilt.
