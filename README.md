# BedrockToolsPlus

## Upstream synchronization

Compatibility signatures, SDK offsets, and interaction-hook ABI are updated from [QYCottage/BedrockTools](https://github.com/QYCottage/BedrockTools) at `3de9a7d641c8222a9bf536d828449a27b58cd20a` (v1.5.8, Minecraft Bedrock `1.26.60.X`). The shader-color and selection-material offsets also incorporate the upstream 1.26.60 rendering fix from `c5bf5347fc8d`. Upstream modules and their overlay bridge are not included; Plus modules can be extended independently. Plus branding, resources, configuration migration, and build optimizations are retained. The upstream MIT notice is preserved in `LICENSE.upstream`; the Plus license remains in `LICENSE`.

Android builds are provided by the build workflow; host tests do not validate offsets or hooks against a running game.

## Introduction

BedrockToolsPlus is an open-source native mod for Minecraft Bedrock on Android, made for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid). It adds a collection of visual, HUD, and utility modules while also providing a small C++ SDK and event system for native mod development.

The source is public so people can study how a real LeviLauncher mod is structured, learn from it, and use the SDK as a starting point for their own mods.

## Features

- Native C++20 mod built for LeviLauncher and Preloader
- 13 configurable modules
- Public headers for Minecraft wrappers, offsets, signatures, and utilities
- Typed event system with runtime subscriptions for other native mods
- LeviLauncher mod-menu integration and persistent configuration
- Open-source and designed to be practical to extend

## Modules

**Visual:** Hitbox, Block Outline, Wings, Jump Circle, Damage Circle, Hit Range

**HUD:** Effect Display, Crosshair, Inventory HUD, Armor

**Misc:** Hit Sound, CommentKey, Command Hotkey

## Hitbox

**Hitbox** draws the collision boxes of nearby entities and players, plus optional eye and look lines, as a world-space overlay.

- Boxes are drawn at the interpolated render position the game draws each entity at, not at the raw collision AABB. The raw box snaps a whole tick of movement ahead of the smoothly moving body, which reads as the hitbox lagging behind the player; **Smooth Boxes** (on by default) derives the interpolation phase from each entity's own box samples, so a stalled tick callback (for example while gliding with a firework-boosted elytra) can never leave a box frozen behind the player. Turning it off gives the plain tick boxes back.
- **Show Entities**, **Show Players** and **Show Items** pick which groups are drawn, **Hitbox Color** and **Show Items Color** style them, and **Line Thickness** above 1 draws real camera-facing geometry because most mobile GLES drivers ignore native line width. Box and line colors remain independent of the game's shader tint in both camera perspectives.
- There is no Range setting any more: the module scans a fixed ~128-block radius around you (far beyond the ~50-block sight the overlay is used at) and draws everything the game fetches there. **Show Entities**, **Show Players** and **Show Items** decide which groups are drawn — turn **Show Items** off if a mob/item farm floods the screen.
- **Show 3rd Person** adds your own box while the camera is behind you, **Show Eye Line** and **Show Look Line** annotate every box, and **Hitbox Indicator** recolors the box under your crosshair with **Indicator Active Color** / **Indicator Default Color**. The highlight follows the game's own crosshair verdict, so an entity lights up only when the game itself says it is within your allowed attack distance (3 blocks in survival, 5 in creative); when that verdict is unavailable the indicator falls back to a 3-block reach measured from your own box.
- Boxes hidden behind solid blocks are culled, so a wall stops an entity's box from showing through it.

## Block Outline

**Block Outline** draws a configurable world-space overlay over the hard-to-see selected-block border. It only follows the block currently under the crosshair and is entirely client-side.

- **Outline**, **Outline Color**, **Outline Opacity** and **Line Thickness** control the 12 block edges. Thickness uses real camera-facing geometry above the hairline setting, so it works on Android GLES drivers that ignore native line width, and the corners stay closed and square at every thickness.
- **Fill** adds a translucent block overlay. **Fill Face Only** limits it to the face under the crosshair; otherwise all six faces are drawn. **Fill Color** and **Fill Opacity** are independent from the outline.
- **Rainbow** animates both passes, with **Rainbow Speed** controlling the cycle. **Pulse** smoothly animates their opacity, with a separate **Pulse Speed**.
- Translucent passes actually blend: any pass with **Opacity** below 1 (or an active **Pulse**) renders with an alpha-blended, still depth-tested material, because the vanilla selection material has no blending state and would ignore the alpha. Fully opaque settings keep the vanilla look.
- **Through Walls** switches to a no-depth material when the current game build provides it. It is off by default, so terrain normally occludes the overlay.
- Outline and fill colors are isolated from the game's shader tint in first and third person; the previous game render state is restored after the overlay.

The target is sampled on the client tick instead of resolving the hit result from the render thread, avoiding frame stalls. Settings and keybind state are persisted in `config.json`.

## Jump Circle

Enable **Jump Circle** to show a soft effect at takeoff and landing, including when walking off a block. **Effect Style** offers 13 shapes: Circle, Square, Star, Triangle, Diamond, Pentagon, Hexagon, Octagon, Four Point Star, Six Point Star, Heart, Flower and Gear; **Effect Duration** controls expansion and fade time (0.1–10 seconds); **Effect Color** sets the color and opacity, while **Rainbow** cycles through RGB colors at **Rainbow Speed** (using the selected opacity instead of the selected RGB color).

**Damage Circle** draws a ring slightly larger than Jump Circle's fully expanded ring by default. Choose from the same 13 **Effect Style** shapes, change **Effect Color** and opacity, or enable **Rainbow** and adjust **Rainbow Speed**. The ring follows the entity—even while moving—and travels from the feet to the top of the hitbox, then back down. **Effect Duration** (0.1–10 seconds) controls only how long the ring remains visible and fades. **Effect Speed** (0.25–4) independently controls how quickly it travels up and down, while **Effect Size** (0.5–2.5) adjusts its fixed diameter. **Show Players** and **Show Mobs** independently enable effects on players and mobs you successfully hit; your own damage effect remains enabled. Effects are local-only.

In **Jump Circle**, enable **Persistent** to keep effects visible after they expand instead of fading. For mobile performance, only the most recent 24 effects are retained; disabling the module or changing worlds clears them. Turning Persistent off allows older effects to expire. The effect is depth-tested and visible in third person.

Jump and landing are inferred from vertical movement, not a verified game-side ground event. Automatic jumps and walking off blocks do not depend on button input, but unusual movement, flight, water, bounce effects, jump boosts or partial-block steps may be missed or mistaken for jumps/landings. Rendering depends on the supported Bedrock version's native signatures and materials.

## Hit Range

**Hit Range** is a native version of the red/green ground-circle indicator shown in the *Hit range+* texture pack: it draws a ring at melee reach so you can see exactly where an attack is possible.

- There is **no Reach slider and no Auto Reach toggle** any more. The module detects Survival versus Creative every local-player tick from the live Player's game-mode object (validated against its attack vtable), with learned vtable identities and debounced non-attack mode events as compatibility fallbacks. It then uses exactly 3 blocks in Survival or 5 in Creative, and a validated mode switch updates the cached radius immediately — without waiting for an attack. Attack-event labels cannot flip the radius during an arm swing. Old `reach` and `autoReach` configuration keys are ignored and removed on save, so the generic menu cannot recreate the removed controls. **Line Width** sets the ring thickness and **Effect Style** offers the same 13 shapes as Jump Circle. **In Range Color** is green and **Out Of Range Color** is red by default; both pickers also control opacity.
- Green now means **inside the automatic 3/5-block 3D reach + an unobstructed visible face**, and it is deliberately **camera/crosshair independent**: turning the screen no longer turns a reachable mob red or an unreachable one green. Distance is measured from the eye to the nearest point on the target's collision box, so vertical offsets are included. The voxel line-of-sight sampler checks five points on each face nearest the eye — including the top/bottom face when the player is above or below the target — and requires one complete face to be clear; voxels overlapping the target's own box are ignored so a supporting/cage block is not mistaken for a wall. Targets behind a wall, too far away, or with only a partially visible face stay red; targets beside or behind the player are still eligible when they are inside reach. The camera-dependent `Level::getHitResult()`/crosshair verdict is not read by this module at all, so there is no exact-aim fallback to flip the colour.
- **Both** your ring and every qualifying target ring turn green together. **Show Targets** draws the same ring around up to eight nearby enemies; out-of-range or hidden rings remain red. **Show Players** and **Show Mobs** choose which groups get a ring, and **Show Self** hides your own ring while keeping the target rings.
- Attack frames keep the rings stable: the radius is taken from the last client tick, centres are clamped against knockback-sized jumps, a brief green dwell suppresses one-frame flicker, and a hit-frame nearby-actor miss holds the last stable target ring instead of dropping it. A confirmed attack never overrides the range/LOS verdict or flashes a false green. **See Through** switches to a no-depth material when the game build provides one; otherwise the ring is depth-tested and terrain occludes it normally. All rings are local-only and never send anything to the server.

When the native solid-block signature is unavailable, the module fails closed to red rather than guessing visibility through walls or trusting a crosshair-only `TypeEntity` result. Nearby actors are sampled on the client tick; self and target rings are interpolated from their last stable position samples. World changes, pauses and disabling the module clear the overlay.

## Inventory HUD

Shows the 27 slots of your inventory grid on the HUD without opening the inventory. Under **Details** you can toggle **Stack Count** and **Durability Bar**, **Number Text** changes the size and color of the stack counts, and **Slot Background** (on by default) draws a cell behind every slot — empty ones included — so the grid reads like the inventory screen. **Background Color** and **Background Opacity** style those cells.

The module owns a single HUD Editor element, **Inventory Grid**. Armor and the offhand are no longer part of it — they live in the separate **Armor** module below.

## Armor

**Armor** is its own module: it draws your helmet, chestplate, leggings, boots and the offhand item on the HUD, with or without Inventory HUD enabled. It has its own toggle, keybind, settings and HUD Editor element (**Armor & Offhand**), so it can be placed anywhere on screen independently of the inventory grid.

- **Offhand Slot**, **Right Hand**, **Stack Count** and **Durability Bar** under **Details** control what is drawn. Like the vanilla inventory, counts are shown only for visible stacks larger than one, and bars only for damaged, damageable items. Enable **Right Hand** to see the count of the stack you are carrying. Count text shrinks when needed to fit small slots.
- **Armor Durability Numbers** is on by default and displays each piece's remaining/maximum durability (for example, `220/363`), including fully repaired armor. It works independently of the durability bars, and its labels are part of the armor element.
- **Slot Background** (on by default) draws a cell behind every armor and offhand slot — empty ones included — so the column reads like the inventory screen. **Background Color** and **Background Opacity** style those cells.
- **Horizontal Layout** lays the five slots out in a row instead of a column.

Configs saved while armor was still an Inventory HUD option are migrated automatically: the new module inherits the position, size and style you had, and it starts enabled only if **Armor & Offhand** was on.

## Crosshair

**Crosshair** replaces the game's own crosshair with one of 30 shapes drawn at the exact screen center. The **Style** picker is grouped by how the shape reads on screen:

- **Marks** — Dot, Plus, X, T Shape, T Shape Down, Chevron, Arrow, Star
- **Gapped crosses** — Cross, Cross Dot, Cross X, Vertical, Horizontal
- **Rings** — Circle, Circle Dot, Circle Cross, Ring Ticks, Broken Ring, Target
- **Boxes** — Square, Square Dot, Diamond, Triangle, Brackets, Brackets Dot, Grid, Square Plus Dot
- **Reticles** — Scope, Mil Dots, Converge

Every shape shares the same controls: **Scale** and **Thickness** set the size and the line weight, **Color** plus **Opacity** style it, and **Outline** adds a dark back-pass so bright skies and sand stay readable. **Rgb** animates the hue through the whole wheel with **Rgb Speed** controlling the cycle. **Indicator** recolors the crosshair (with **Indicator Color**) while you are aiming at a mob or another player — the hit test lives in this module, so it works without enabling Hitbox. **Show Third Person** also draws the overlay while the camera is behind or in front of you; it is off by default, like vanilla.

Selecting **Vanilla** gives the crosshair back to the game: the module then only tints the game's own crosshair when the indicator fires (and, on builds that cannot be tinted in place, briefly swaps it for a same-shaped overlay), so exactly one crosshair is ever on screen. New styles are always appended to the picker, so configs saved by an older version keep drawing the same shape.

## Wings

The **Wings** module renders animated 3D wings on your back that flap, idle and glide with your movement. Open the module's **Wing Style** selector to choose a shape:

- **Dragon** — the default: a broad bone-framed membrane wing
- **Angel** — layered white primaries with gold tips
- **Demon** — black bones with a glowing blood-red membrane
- **Bat** — a small, dark violet membrane wing
- **Butterfly** — rounded pink and blue lobes
- **Phoenix** — long fiery orange primaries
- **Fairy** — small translucent cyan/pink wings
- **Demon Wings** — larger black-framed crimson wing with a glow aura
- **Vampire Bat** — a narrower crimson membrane with deeper fingers
- **Red Bat** — the shortest, most saturated bat wing

**Motion:** the idle → flap → glide transitions are continuous ramps. Speeds measured from the player's tick-quantised AABB are low-pass filtered, the blend weights ease toward their targets with a real time constant, and the render thread lerps them between the last two ticks by the partial-tick fraction. A vanilla jump no longer flips the pose (idle → flap → glide → idle) in one 50 ms tick, which used to pop the wings by tens of degrees in a single frame. Short hops never enter the spread glide pose; gliding needs a sustained descent. The wing anchor is interpolated from the last two AABB samples the same way the game interpolates the player mesh, using the same sample history as Hitbox, so walking and jumping stay smooth at any frame rate. `tests/wings_patch_test.cpp` drives a 60 fps jump through `WingsModule::boneAnglesAt()` and asserts the shoulder angle never jumps more than 6°/frame (it was ~36°/frame before this fix).

![Wing pose continuity before and after](docs/wings-animation-smoothness.png)

*Shoulder angle over one jump: the old 0/1 pose switch produced a one-frame
pop; the continuous blend stays within the natural flap speed.*

The wings are a world-space overlay (a `RenderLevel` hook + tessellator); they never touch skin memory and only appear from a third-person point of view. They are drawn with a depth-tested game material, so your body correctly occludes them when the camera swings in front of you, and the side facing the camera is auto-selected: the bright membrane from behind, a darker shadowed underside from the front. Every style is one continuous membrane instead of a chain of boxes: an articulated arm (shoulder, elbow, wrist) is bent into a smooth leading edge, and a cambered sail hangs off it with a swept, scalloped trailing edge, finger struts, a spar and wing-tip claw, a shoulder fairing and — on the feathered styles — three overlapping rows of quills. The sheet ripples while you flap and goes taut while you glide, and it is shaded per face with a headlight plus a sky lift so it keeps its volume from every angle.

![Wings before and after](docs/wings-before-after.png)

*Top row: the old per-bone box renderer. Bottom row: the membrane wing
(Dragon, Angel, Demon).*

Developers can preview every style offline with `./scripts/gen_wings_preview.sh`, which writes a pose sheet, a close-up and a showcase of all styles to `build/wings-preview/`. The geometry is pure C++ (`src/modules/visual/wings_design.hpp` + `wings_surface.hpp`), so the preview and `tests/wings_shape_test.cpp` rasterize/assert exactly what the game draws.

## Hit Sound

The **Hit Sound** module plays a custom sound of your choice every time you land a melee hit on a mob or another player.

1. Put sound files (`.wav`, `.ogg`, `.mp3`, `.m4a` or `.flac` — ideally short one-shot effects; a `Sample Hit.wav` is generated for you on first launch) into the `hitsounds` folder next to your `config.json` (`<mod config dir>/hitsounds`, created automatically on first launch).
2. (Re)launch the game, open the BedrockToolsPlus mod menu and enable **Hit Sound**.
3. Pick a file in the module's **Sound** selector — every audio file in the folder shows up there. Choose `None` to keep the vanilla behavior. The **Volume** slider sets how loud the sound plays.

The sound is a purely client-side overlay: the victim's own hurt sound is not cancelled or replaced, and nothing is sent to the server. Files whose names contain a comma are ignored (the menu picker cannot represent them), and sounds that fail to decode on your device are simply skipped.

## System Requirements

- Android 9 or newer
- 64-bit ARM device (`arm64-v8a`)
- [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid)
- Minecraft Bedrock **1.26.50** (`1.26.50.4`) — the version declared in `levimod.json`; other builds may work but are untested

## Installation

1. Install LeviLauncher.
2. Download the latest `BedrockToolsPlus.levipack` release.
3. Import the package from LeviLauncher's mod manager and enable it.
4. Launch Minecraft through LeviLauncher.

## Development Setup

Requirements:

- Android NDK r28c
- xmake
- Python 3

Build for Android ARM64:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The release build produces `libBedrockToolsPlus.so` and `BedrockToolsPlus.levipack` in the xmake target directory.

Public SDK headers are under `include/bedrocktools`. Shared runtime code lives under `src/core`, while features are kept under `src/modules` by category. Minecraft signatures and offsets are version-specific, so those are the main pieces that normally need updating for a new game build.

Example event subscription from another native mod:

```cpp
#include <bedrocktools/BedrockToolsPlus.hpp>

bedrocktools::events::RuntimeListener<bedrocktools::events::LocalPlayerTickEvent> listener(
    [](auto& event) {
        if (!event.player) return;
        auto position = event.player->position();
    }
);
```

## Contributing

Pull requests are highly appreciated. Keep changes focused, preserve the existing project structure, and test changes against the intended Minecraft version before submitting them.

## Usage Guidelines

Do not use LeviLauncher or BedrockToolsPlus to violate Mojang or Microsoft's user agreements.

**Disclaimer:** The authors and contributors of BedrockToolsPlus and LeviLauncher are not responsible for bans, damages, or issues arising from the use of this software. Use it at your own risk and in accordance with Minecraft's terms of service.

## Credits & Acknowledgements

BedrockToolsPlus is made by [RadiantByte](https://github.com/RadiantByte) and maintained by VENOM P2 GM.

Special thanks to [dreamguxiang](https://github.com/dreamguxiang) for helping make this mod possible.

Motion blur module based on [mcpelauncher-motion-blur](https://github.com/CrackedMatter/mcpelauncher-motion-blur) by [CrackedMatter](https://github.com/CrackedMatter).

Thanks to [Kashifro](https://github.com/Kashifro) for the Shulker Preview and Tablist modules.

Built for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid).

## Contact

Discord: [discord.gg/rMgdpTFFVg](https://discord.gg/rMgdpTFFVg)

**Report Issues:** Open an issue in this GitHub repository.

## License

BedrockToolsPlus is licensed under the [GNU General Public License v3.0](LICENSE).
