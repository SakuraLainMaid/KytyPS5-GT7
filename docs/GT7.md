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

In the game's own graphics options, use the **performance (frame rate) mode**.

The ray tracing (RT) mode looks right once its shaders are compiled (see What's new), but it is
**not recommended for playing yet**: its frame rate is low (about 10 to 20 fps in the garage), and
races in it are untested. It needs **Bindless textures** on in the launcher.

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

## What's new (October 22, 2026)

**All shaders are compiled again after this update** (pop-in the first time in each place, and a
few minutes for each car's first visit in ray tracing mode).

- **Ray tracing mode looks right** (on an RTX 5090 too): no more blotchy interiors, white glow on
  the wheels or rainbow colors. These came from the cars' material shaders, which are very big:
  each one takes 20 to 35 seconds to compile, and a car needs about 100 of them. Until a
  material's shader was ready, that part of the car was drawn wrong. Now:
  - The emulator's console window shows how far they are, every 10 seconds
    (`Shaders compiling in the background: ...`), and says `all done` at the end.
  - More of them compile at once on CPUs with many cores (up to 16 with 32 threads and 48 GB of
    memory or more), at a lower priority than the game, and none sits waiting while there is
    room.
  - A car's materials are compiled once. Before, the game loading the same car again at another
    place in memory (each garage visit could) made the emulator compile them all again.
  - **Tip:** the first time you look at a car in ray tracing mode, wait until the console says
    `all done`. The car repairs itself while you look at it; you don't need to leave the garage.
- **No more crash in races** with `resource.mip_count` in the error
  ([issue 3](https://github.com/usa-ratt/KytyPS5-GT7/issues/3),
  [issue 9](https://github.com/usa-ratt/KytyPS5-GT7/issues/9)). The game asked for a texture of 7
  mip levels whose description arrived half empty; the emulator puts in an empty stand-in for it,
  and the shader that reads it by mip level now gets the stand-in at every level instead of the
  emulator stopping.

## What's new (October 21, 2026, fourth build)

No shaders are compiled again after this update.

- **Music plays** ([issue 6](https://github.com/usa-ratt/KytyPS5-GT7/issues/6)), in the menus and
  everywhere else. The game plays its music through a kind of audio channel the emulator dropped;
  those channels are now mixed into the sound output (from upstream KytyPS5's work by nmzik and
  chenxiao07), with the game's own volume settings for each.
- **Car photos and pictures everywhere** ([issue 7](https://github.com/usa-ratt/KytyPS5-GT7/issues/7)):
  the used car dealer's photos and the other pictures in the menus that were missing. They are
  JPEG pictures in the game's files, and the emulator had no JPEG decoder for the game to use.
- **No more crash in time trials** ([issue 8](https://github.com/usa-ratt/KytyPS5-GT7/issues/8)).
  A draw pointed the emulator at vertex data outside the game's memory, and reading it ended the
  emulator. Such a draw is now skipped.

## What's new (October 21, 2026, third build)

No shaders are compiled again after this update.

- **No more freeze when you pick Exit after finishing a license test.** At that moment the game
  points some graphics memory at the start of its own program code. To keep track of that
  memory, the emulator changed the code's memory protection, and every game thread that ran
  that code got stuck. The emulator now leaves the game's code and stack memory alone.
- **Fewer crashes in license tests and at retry prompts.** A helper thread that reads the game's
  graphics commands ahead of time could still read commands from memory the game was freeing or
  reusing, and crash on them. It now works on its own safe copy, checks every register a command
  sets, and throws away a copy made after the game could have reused that memory.
- When the graphics driver itself crashes (one of the crashes in
  [issue 3](https://github.com/usa-ratt/KytyPS5-GT7/issues/3)), the crash report now names the
  graphics buffer the GPU tried to read, and whether and when it had been freed.

## What's new (October 21, 2026, second build)

After updating to this build, the first session compiles every shader again (pop-in everywhere,
once).

- **The spectators in the stands all have their colour now**, in the shade too. Each spectator
  looks up its own light probe (a small record of the light around it) by a number, and that
  number reached the shader through a slot the emulator never filled: the spectators lit only by
  their probe were drawn black. Other shaders that read data the same way are fixed with it.
- **No more crash at the end of a replay** ("cmd_offset >= Pm4::SH_NUM"). A helper thread that
  reads the game's graphics commands ahead of time read some that the game was already
  overwriting, and stopped the emulator; it now checks a copy.
- Smoke drawn as a solid blue or black blob (rare): the emulator cleans invalid numbers out of
  the light probes the smoke is lit from, but the biggest probe arrays were too big for that
  check. They are checked now.
- More information in the log for bug reports about missing music.

## What's new (October 21, 2026)

- **The lines on your car's windows in the race countdown and in replays are gone.** Those views
  are drawn at half resolution with two depth samples a pixel and one colour sample, and on
  NVIDIA cards both triangles that share an edge coloured the pixels along it, so the glass
  darkened them twice. Each pixel now goes to one triangle. The first countdown and replay after
  updating build their shaders once more (a little pop-in there, once).
- **About 1 to 2 fps more in races** (21 instead of 20 at Tokyo against AI cars on the PC above,
  and steadier). Small jobs the game hands the graphics card, and waits for, no longer wait
  behind long stretches of drawing. `KYTY_COMPUTE_YIELD=0` turns this off, for comparison.
- A compute shader the graphics driver cannot build is skipped instead of ending the emulator.
- More information in the log for bug reports (what the game tries to download, and the files
  it looks for that are not there).

## What's new (October 20, 2026)

After updating to this build, the first session compiles every shader again (pop-in everywhere,
once).

Ray tracing mode (still not recommended, see Known problems):

- **The blocks in the reflections are gone, and the mode runs several times faster.** The game
  unmaps the memory that holds the cars' ray tracing data and maps it again as it loads a car,
  and the emulator could then keep an old copy of that data on the graphics card. Groups of rays
  then walked lists that never ended until a limit stopped them: the blocks, and 1 to 3 fps. In
  the garage the mode now runs at about 15 fps in the cockpit view and 25 or more outside, on
  the PC above.
- **The reflections are shaded and have their textures.** Their material passes never ran
  before, and their textures were all looked up as the same empty one.
- **The first visit compiles the reflections' materials** in the background: a few minutes for a
  car seen for the first time. Until a material is ready, its reflections are missing or flicker.
  Close the game normally afterwards so they are saved; the next visit has them from the start.
- Closing the game in this mode no longer hangs ("not responding"): it waits at most 20 seconds
  for materials still compiling, and those compile again next time.

Crashes and hangs:

- **No more crash as some races load (Tokyo), or on the Mazda LM55 VGT's showroom page**
  ("Not implemented ... user_clip_planes"). The game sets user clip planes, which the emulator
  now draws without instead of stopping.
- **A loading screen that never ended is fixed** (about one track load in ten, the first one
  after the start too). The game needed more room past its own memory than the emulator gave
  it: 16 GB now, 8 GB before.
- With bindless textures on, a texture slot the game is rewriting no longer ends the emulator
  ("unknown format", "texture base layer is out of bounds").
- If the graphics driver fails to build a shader, the crash report now names the shader and the
  driver's answer.

Graphics and shaders:

- **Most spectators in the stands have their colour now**, and the red and white barrier that
  was drawn wrong on the track is right. The list of people to draw is built with a GPU
  instruction mode the emulator did not implement (DPP row share).
- **Bindless textures are now the recommended setting** (launcher, game settings). It costs
  nothing measurable in a race, and the ray tracing mode needs it.
- **Compiled shaders are saved again once the cache is big.** The graphics driver's shader cache
  file had reached the 3.75 GB a file may hold, and after that nothing new was saved, so every
  session compiled the same shaders again. It is now kept in up to three files.
- The car views shown before an event no longer go black for a long time (up to 50 seconds in a
  session) while their shaders compile: they compile in the background.

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

## Known problems

- **Ray tracing mode:** the frame rate is low (about 10 to 20 fps in the garage). A car looks
  blotchy for the few minutes its material shaders compile the first time (see What's new). The
  reflections can still flicker a little and look a bit jagged, most of all right after switching
  between the cockpit and outside views.
- Tokyo: at some times of day and weather (twilight and night), the picture turns green at some
  tunnel exits and under some overpasses, most often in replays, and fades back once past them.
- In replays, small red lights can show through walls.
- A graphics driver crash, seen after many license test retries
  ([issue 3](https://github.com/usa-ratt/KytyPS5-GT7/issues/3)), is not fixed yet. If it happens
  to you, please add this build's crash report and the zipped log to that issue: the report now
  names the graphics buffer involved.
- In-game videos can play out of sync with their sound
  ([issue 4](https://github.com/usa-ratt/KytyPS5-GT7/issues/4)). Being looked into.
- Once, in a race on a new track in the chase view, most of the track disappeared after it had
  loaded (cars and some objects stayed); switching to the cockpit view and back brought it back.
- If a loading screen still never ends, please open an issue with the log.

## Current state

As of October 2026:

- The game reaches its menus and runs races.
- Frame rate at 3840x2160 on the PC above: about 19 to 24 fps in a race against AI cars at Tokyo;
  in a time trial at Tokyo, high 30s on average, mid 50s in some sections.
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
- Known graphics problems: the FINISH and retry screens are drawn wrong.
- The reflections' light lists sometimes hold corrupt cells (cells asking for thousands of lights).
  Those cells are emptied before they reach the graphics card; without that, they caused freezes of
  tens of milliseconds. Their cause is still being investigated.
