# Element 115 (engine / harness)

Unofficial, personal-use native engine port that runs the zombies mode of a 2010 first-person shooter on arm64 (macOS and Android) with its own Vulkan backend.
It is an independent project and is not affiliated with or endorsed by the original game's publisher or developer.

**No game data is included or distributed.** To run it you need your own legally obtained copy of the game files; nothing from the game is stored in this repository.

## What this repository is
The *harness*: client-side layer (HUD, touch controls, menus, audio, loading video, per-map client scripts) that sits on top of the game-logic port in the companion repository `element-115-zdec`.
It is **not buildable on its own**: it expects the rest of the backend (D3D9-to-Vulkan layer, shaders, compatibility headers) that lives outside this repository and has not been published yet.

## Related repositories
- `element-115-android`: Android app and packaging scripts.
- `element-115-zdec`: game-logic port (fork of an existing public decompilation project; see its `PROCEDENCIA.md`).

## Third-party code
Video and audio decoding use libvpx (BSD) and libopus (BSD); their notices are listed in `element-115-android`.

## License
No license has been chosen yet; until one is added all rights are reserved by the author.
