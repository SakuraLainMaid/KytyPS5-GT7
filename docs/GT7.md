# Gran Turismo 7 on this branch

This branch (`gt7-eqaa`) carries work on getting Gran Turismo 7 playable. It builds on the
`wolverine` branch of [Senaxx/KytyPS5](https://github.com/Senaxx/KytyPS5). Build it as described
in the [README](../README.md#building); nothing extra is needed.

Tested with:

- Game: PPSA01317, version 01.710, from a legally obtained dump. Other versions are untested.
- PC: Windows 11, NVIDIA RTX 5090, AMD Ryzen 9 7950X3D.

## Launcher settings

These are the settings the game was tested with, in the launcher's **Edit game config** dialog for
the game. The user profile, audio and notification settings don't matter.

| Section | Setting | Value |
| --- | --- | --- |
| Graphics & display | Screen resolution | 3840x2160 |
| | Present mode | Mailbox |
| | Vblank frequency | 60 |
| | Shader optimization | Performance |
| | GPU | Auto |
| | Fullscreen | Off |
| | Enable readback | Off |
| | Enable tessellation support (experimental) | **On** |
| | Auto-hide cursor | Off |
| | Bindless textures (experimental) | Off |
| | Skip shaders | Empty |
| Compatibility | AMD CPU patch (experimental) | Off |
| | Windows SysV red zone crash protection (experimental) | **On** |
| Debugging & logs | Vulkan validation | Off |
| | Shader validation | Off |
| | RenderDoc capture | Off |
| | Command buffer dump | Off |
| | Shader logging | Silent |
| | Printf output | File (`_kyty.txt`) |
| | Tracy profiler | **Off** (it costs frame rate) |

## In-game settings

In the game's own graphics options, use the **performance (frame rate) mode, not the ray tracing
(RT) mode**. The ray tracing mode isn't supported properly yet.

## Notes

No environment variables are needed to play. The `KYTY_*` variables mentioned in the commit
history are for diagnostic runs only.

The first time you reach a new area, the game stutters while its shaders are translated and
compiled. They are cached, so later runs through the same area are smoother.

## Current state

As of October 2026:

- The game reaches its menus and runs races.
- Frame rate at 3840x2160 on the PC above: about 27 to 30 fps standing still at Tokyo, and about
  13 to 16 fps in a race against AI cars there.
- The cars' bodies, the AI cars' and the player's, are drawn right in the cockpit and chase views.
  The game skips the parts of a car it finds hidden by testing them against a small depth picture
  of the scene, and that test used to read the picture at the wrong place, so parts went missing or
  black. The first time you race after updating, expect a short stutter while the fixed shaders are
  compiled.
- Known graphics problems: a thin spike sticks out of the roof of the player's car in the chase
  view; lines show on the player's car's glass during the race countdown and in replays (it looks
  right while driving); the FINISH and retry screens are drawn wrong.
- The reflections' light lists sometimes hold corrupt cells (cells asking for thousands of lights).
  Those cells are emptied before they reach the graphics card; without that, they caused freezes of
  tens of milliseconds. Their cause is still being investigated.
