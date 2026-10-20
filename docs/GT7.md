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
| | Bindless textures (experimental) | **On** |
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

**Pop-in on new tracks is expected.** The first time you drive a track (or a part of one), its
shaders are compiled while you drive, in the background: scenery, cars' details and reflections
appear a moment late instead of the game freezing while they compile. The compiled shaders are
cached, so the second time on the same track things are there from the start. On a fast PC most
of a new track is in within half a lap; on slower PCs it takes longer. After a graphics driver
update everything is compiled once more.

To report a problem, open an issue on this repository with the **Bug report** form (attach the
zipped `_kyty.txt` log).

## What's new (October 19, 2026)

- **No more long freezes on new tracks.** Driving onto a part of a track the game had not loaded
  yet froze the picture for seconds to more than a minute (audio kept playing), and some loading
  screens never finished. The game maps memory thousands of times while it streams a track in,
  and the emulator's record of the game's memory was rebuilt on every change; each change now
  costs microseconds instead of milliseconds. In one race on a new track the picture stood still
  in 248 of 449 two-second stretches before, and in 1 of 144 after.
- **Shader compiling no longer stops the game.** New shaders for the cars' reflections, and for
  other passes the game redraws every frame, were compiled while the game waited, about a second
  each; they are compiled in the background now (that is the pop-in above). The emulator also asks
  the graphics driver first whether it already has a compiled shader, and no longer waits on the
  ones it has to compile.
- **The launcher tells you when a new build is out**, when it starts (untick "check for updates
  on startup" to turn it off).
- **Reflections every other frame (faster)**: a launcher option (game settings) that redraws the
  cars' reflections every other frame instead of every frame: about 1 fps more in a race, about
  4 fps more in a time trial, with no difference we could see.
- The thin spike that stuck out of the cars' roofs is gone (all cars).
- A crash when the emulator wrote a depth image back to memory while freeing it is fixed (seen in
  My First Gran Turismo with bindless textures on).
- Races that need more memory than the game's own get up to 8 GB more room past it (4 GB before).
- **Bindless textures are now the recommended setting** (launcher, game settings): it costs
  nothing measurable in a race, and the ray tracing mode needs it (without it the reflection
  passes of that mode never run). A texture slot the game is rewriting no longer ends the
  emulator ("unknown format", "texture base layer is out of bounds").
- **The ray tracing mode's reflections are shaded** (garage tested), and their materials have
  their textures now: before, each of these textures was looked up as the same empty one, and
  drew as a placeholder. Each reflection pass is compiled in parts, one for each car material,
  in the background, and drawn once for each part: a pass with all of a car's materials in one
  took the graphics driver many minutes, and in a ray-traced race preparing it froze the game for
  minutes and kept it from closing. A material whose part is still compiling is missing from the
  reflections until it is ready. Close the game normally afterwards: the compiled parts and the
  materials they need are saved, and the next visit has them at once.
- After updating to this build, the first session compiles every shader again (pop-in
  everywhere, once).

Known problems in this build:

- Ray tracing mode: switching between the interior and exterior views in the garage can leave
  white wheels, red pixels or blocky patches in the reflections for a while.

- Rarely, after several races in one session, a loading screen keeps spinning and never ends.
  Restart the game.
- Once, in a race on a new track in the chase view, most of the track disappeared after it had
  loaded (cars and some objects stayed); switching to the cockpit view and back brought it back.

## Current state

As of October 2026:

- The game reaches its menus and runs races.
- Frame rate at 3840x2160 on the PC above: about 18 to 22 fps in a race against AI cars at Tokyo,
  high 30s to near 50 fps in a time trial.
- On a Ryzen with 3D V-Cache on one of its two dies (such as the 7950X3D), the emulator's
  graphics threads start on the V-Cache cores by themselves (about 2 fps in a race). Starting the
  emulator with an affinity of your own (Process Lasso, for example) keeps yours, and
  `KYTY_CPU_SET=off` turns it off.
- Texture descriptors no texture can be, which ended the emulator in some races and in the
  cockpit view ("unsupported texture mip view changes physical layout"), are drawn as empty
  textures. A race whose loading needs more than the game's 12.5 GB of memory no longer crashes:
  the emulator gives it room past that (the log says "did not fit" when it does).
- The cars' bodies, the AI cars' and the player's, are drawn right in the cockpit and chase views.
  The game skips the parts of a car it finds hidden by testing them against a small depth picture
  of the scene, and that test used to read the picture at the wrong place, so parts went missing or
  black. The first time you race after updating, expect a short stutter while the fixed shaders are
  compiled.
- The thin spike that stuck out of the roof of the cars in the chase view is gone: the last bytes
  of a table the cars' glass reads were read as zero.
- Known graphics problems: lines show on the player's car's glass during the race countdown and in
  replays (it looks right while driving); the FINISH and retry screens are drawn wrong.
- The reflections' light lists sometimes hold corrupt cells (cells asking for thousands of lights).
  Those cells are emptied before they reach the graphics card; without that, they caused freezes of
  tens of milliseconds. Their cause is still being investigated.
