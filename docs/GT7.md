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

Since the October 25 build the launcher starts with these settings, except the screen ones
(resolution, present mode, vblank frequency, fullscreen), which depend on your screen. A
configuration saved with an earlier launcher is switched to them once (see What's new); what you
change after that is kept.

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

## What's new (October 26, 2026)

No shaders are compiled again after this update.

- **A crash in races fixed.** The game rewrites its texture list while it streams textures in. The
  emulator could read an entry the game was halfway through rewriting, which then pointed past the
  end of the game's memory, and it stopped. Such an entry now gets a placeholder texture for a
  moment and is read again later, as other half-written entries already were. This was rare and
  depended on timing, so it could happen on earlier builds too.
- **Graphics cards without fragment shader barycentrics get past start-up**
  ([issue 20](https://github.com/usa-ratt/KytyPS5-GT7/issues/20)): Intel Arc, GeForce GTX 10xx and
  Radeon RX 5000 cards, and older drivers. From upstream KytyPS5 (Matthew Hurley and nmzik). The
  emulator shows a warning at start-up on those cards. Whether the game runs on them is not known
  yet: a shader that needs the feature can still fail there.
- **Less waiting on the emulator's GPU thread.** When the game reads something the graphics card
  wrote, the copy is now waited for by the thread doing the read instead of the GPU thread, and
  each read waits only for its own copy (`KYTY_FAST_READBACK=gpu` brings back the old way, for
  comparison). Smaller cuts: a draw's shader stages take their prepared work without a lock, the
  GPU thread does the page protection work itself instead of waiting for the helper thread to wake
  up, and texture lookups go through a small quick-lookup table first. In test races the GPU
  thread spent less time on reads, but the frame rate stayed about the same: the GPU thread
  handles every draw and compute job the game sends (about 3,700 a frame), and that is the limit
  now.

## What's new (October 25, 2026)

No shaders are compiled again after this update.

- **AMD graphics cards: no more crash at the GT logo**
  ([issue 21](https://github.com/usa-ratt/KytyPS5-GT7/issues/21)). The AMD driver crashed on one
  part of the emulator's mesh shaders (they draw the game's text and a few other things): their
  Layer output, which says which layer of a multi-layer picture to draw into. The game's menus and
  races draw into one-layer pictures, so on AMD cards it is now left out (`KYTY_MESH_LAYER=keep`
  keeps it). Found with the test settings of the third October 24 build; thanks to Shuyuyus for
  running them. Other AMD problems may still come up further in: please report them.
- **The recommended settings are now the defaults**
  ([issue 25](https://github.com/usa-ratt/KytyPS5-GT7/issues/25)). New launcher configurations
  start with the tested settings above. A configuration saved with an earlier launcher is
  switched to them once: tessellation, bindless textures, red zone protection and the `_kyty.txt`
  log file on, readback and the debugging options (validation, RenderDoc, command buffer dump,
  Tracy) off, shader optimization Performance. Resolution, present mode, vblank frequency and the
  other personal settings stay, and what you change after that is kept.
- **Red zone protection is on by default in the emulator too** (`--no-redzone` turns it off).
  Without it, a race could crash as it started (issue 25): the game keeps a few values just below
  its stack, and Windows writes over them each time the emulator catches a memory access.
- **A fix from upstream KytyPS5** (Nawid3333): the code that answers the game's thread-local
  storage reads could change the processor's compare flags, so in rare cases the game took the
  wrong branch right after one of those reads.
- **A faster Windows build:** it is now compiled for AVX2 processors (every PC that runs the game
  has AVX2) and optimized from a recorded race (profile-guided optimization). In a test race the
  emulator's GPU thread took roughly a fifth less time a draw, but the frame rate rose only 1 to 2
  fps: the game's own main thread holds it back now too.

## What's new (October 24, 2026, third build)

No shaders are compiled again after this update. Nothing changes unless the test setting below is
set.

- **AMD graphics cards:** a new test setting for the crash at the GT logo
  ([issue 21](https://github.com/usa-ratt/KytyPS5-GT7/issues/21)). The AMD driver crashes while
  it compiles the emulator's mesh shaders (they draw the game's text and a few other things).
  `KYTY_MESH_TEST` (`empty`, `noguest`, `nolayer`, `nocull` or `fixedcount`) takes one part out of
  every mesh shader before the driver sees it, to find the part the driver fails on. The steps are
  in issue 21. With the setting on, the text is missing or looks wrong.

## What's new (October 24, 2026, second build)

No shaders are compiled again after this update.

- **No more crash at a race start with a vblank frequency of 120**
  ([issue 22](https://github.com/usa-ratt/KytyPS5-GT7/issues/22)). With 120 set in the launcher,
  the game offers its 120 Hz option; with it on, the game switches its output picture to
  2240x1260 when a race starts, in a compressed layout the emulator did not accept yet ("unsupported
  or invalid video-out surface attributes"). It does now. The 120 Hz mode doesn't raise the frame
  rate in the emulator (about the same as at 60 Hz in a test), so 60 stays the recommended
  setting.

## What's new (October 24, 2026)

No shaders are compiled again after this update.

- **No more long freeze or black screen at the start of a race while shaders compile.** On a
  fresh shader cache (the first race after an update that resets shaders, a new track, a new car)
  the screen could stay black after the countdown for minutes, or freeze for about a minute before
  it. Three causes, all fixed:
  - Some draws can't be skipped, so the emulator stops and waits for their pipeline. Since the
    eighth build the compile workers took old requests only on two threads, and while the emulator
    waited nothing new was asked for, so the pipeline it waited for sat behind hundreds of others
    (190 s for one pipeline in a test). The pipeline the emulator waits for is now compiled next.
  - While the emulator waits, every compile worker now helps with the queue (the game is stopped
    then anyway), instead of 2 of them.
  - The reflection cube (the picture of the surroundings the cars reflect) is drawn again every
    frame, so its draws may now be skipped while they compile from its very first frame. Before,
    the race start compiled them one by one (33 in a row, 26 s, in a test). On a fresh cache the
    reflections can miss some scenery for a few seconds at the start instead.
- **AMD graphics cards:** a test setting for the crash at the GT logo
  ([issue 21](https://github.com/usa-ratt/KytyPS5-GT7/issues/21)): `KYTY_PS_BARYCENTRICS=0`
  leaves out the barycentrics that the pixel shader of the crashing pipeline asks for
  (`KYTY_PS_BARYCENTRICS=mesh` only next to mesh shaders). It changes nothing without the setting.

## What's new (October 23, 2026, eighth build)

The shaders are compiled again once after this update (the shader translator changed): the first
menu and the first race take longer than usual.

- **The thin line up the sky is gone, and the track is one shade on both sides of it.** The sky
  and the reflections come from cube maps (six square faces). The emulator samples each face as a
  flat picture, and with the game's "wrap" setting the filtering at a face's edge blended in the
  opposite edge of the same face: a thin line along each face edge, and a step in the track's
  reflection. The console ignores that setting for cube maps; the emulator now keeps each face's
  filtering inside the face (`KYTY_CUBE_EDGE_CLAMP=0` turns it off).
- **Fixes from Franseven's review of the shader translator** (thank you), several ported from
  mainline (nmzik, Mr. Big Britches, Andre Michi, Nawid3333): NaN and denormal handling, branches
  of wave64 shaders on 32-lane cards with missing lanes, V_READFIRSTLANE with no active lane,
  saturate and float-to-integer clamping, bit-field inserts in descriptors, cube coordinates,
  more texture formats sampled (draws using formats 30 and 32 are no longer skipped), textures
  with an invalid mip range read as empty instead of skipping the draw, and the same SPIR-V on
  every run.
- **Grass after a race, then a Time Trial, is right** (Franseven): an image written back to
  memory now makes the buffers over the same memory upload it again.
- **Cards and tools without mixed-sample support** (RenderDoc; Intel Arc too, most likely) no
  longer end the emulator at the start of a race (Franseven): those draws render their colour at
  the depth's sample count and resolve it.
- **Objects that were still compiling no longer stay missing** (Franseven): a draw skipped while
  its pipeline compiles counts as visible to the game's visibility tests, so the game keeps
  drawing it. Pipeline compiles now go to what is on screen first, and the backlog runs on two
  low-priority threads instead of slowing the game.
- `KYTY_PIPELINE_QUICK=1` (off by default) draws a new object at once with an unoptimized
  pipeline while the optimized one compiles: no invisible cars on a fresh cache, at some cost in
  frame rate until the optimized ones are in.

## What's new (October 23, 2026, seventh build)

No shaders are compiled again after this update.

- **Track maps on the layout selection screen are right.** When the list scrolled, a slot could
  show two layouts' maps on top of each other, or none. The game draws every map into the same
  small picture, one after another, and clears it before each one; the emulator checked such a
  clear only once per batch of graphics work, so when two maps fell in the same batch the
  second clear was skipped. It is now checked again whenever the game clears the picture again.

## What's new (October 23, 2026, sixth build)

No shaders are compiled again after this update.

- **No more crash in some replays** ([issue 18](https://github.com/usa-ratt/KytyPS5-GT7/issues/18)).
  One tessellated draw in a replay used a setup the emulator doesn't support yet, and the
  emulator closed. That draw is now skipped (something in the replay may be missing for a
  moment), and the log names the setup ("Tessellation: ... not supported").
- **A clearer start-up error on graphics cards that lack something the emulator needs**
  ([issue 20](https://github.com/usa-ratt/KytyPS5-GT7/issues/20)): it now lists everything the
  card is missing, not only the first thing.

## What's new (October 23, 2026, fifth build)

No shaders are compiled again after this update. A few pipelines (draws that use
alpha-to-coverage) compile once, so the first race after updating can stutter a little.

- **Smoother races.** In a 5-lap race at Tokyo twilight against 11 AI cars the frame rate stayed
  around 22 to 23 fps, rarely below 21 and often 23 to 24, in spots that used to drop to about 18.
  What changed:
  - When several of the game's threads read the same results of the graphics work at once, the
    ones that find the data already fetched go on at once instead of waiting for the area around
    it to download again.
  - The memory tracker's locks are held much less, and memory protections from different threads
    no longer queue behind one another (Franseven's ports of mainline changes).
  - Fewer full GPU barriers (Franseven's port of a mainline change).
- **Alpha-to-coverage** is now honoured: the edges of foliage, fences and other cut-out textures
  are drawn the way the game asks (from mainline, ported by Franseven). `KYTY_ALPHA_TO_COVERAGE=0`
  turns it off.
- **The game's online requests fail the way they do on a PS5 with no internet.** Before, each one
  failed with an emulator error (a bad handle). The emulator still never goes online.
- The game's checks for a racing wheel now get a proper "no wheel connected" answer.
- More fixes from Franseven's batch for other games: audio contexts no longer stall each other,
  network functions some games call, images whose creation the driver refused, and an audio
  format some games pass the other way round.

## What's new (October 23, 2026, fourth build)

No shaders are compiled again after this update.

- **No more hard freezes at the start of a race.** The emulator read each shader the game used for
  the first time in a session from its shader cache on disk on the same thread that runs the
  game's graphics work. On some runs each read took 7 ms, and the start of a race reads about
  3,000: 22 seconds of the race, most of it in lap 1. These reads now run on 4 helper threads, and
  a draw whose shader is still being read is left out for a frame or two instead.
- **No more "the whole track flickers away" at the start of a race.** When hundreds of new
  pipelines are looked up at once, a frame could be drawn with most of the track missing. Such a
  frame (one that left out 100 draws or more) is no longer shown; the frame before it stays on the
  screen instead. `KYTY_HOLD_SKIPPED_DRAWS=0` shows every frame again.
- **Compiled pipelines are kept after a crash.** They were saved only when the emulator closed
  normally, so a crash threw away every pipeline the session had compiled, and the next session
  compiled them all again (thousands, on a new track). They are now also saved every 5 minutes
  while you play. `KYTY_PIPELINE_SAVE_MINUTES` sets how often (0: only when closing).
- **Crash reports say more.** A crash inside the graphics driver while it compiles a pipeline
  names the pipeline, and a crash in the game's own code shows the state of the exact memory it
  was using. The launcher's console window now prints the emulator's exit code when it ends.

## What's new (October 23, 2026, third build)

No shaders are compiled again after this update.

- **Faster races, about 1.5 fps.** Two settings that won in A/B tests are now on by default, measured
  in a 5-lap race at Tokyo against 11 AI cars:
  - Memory protection changes the graphics work asks for now run on a helper thread, instead of
    holding up the thread that runs the game's graphics work: +0.6 fps.
  - Draws go to the graphics card in batches of 64 instead of 16: +1.0 fps.
- **Fewer memory protection round trips.** When the emulator writes a small value into memory the
  game is watching (mostly "done" markers), it now writes it in place instead of unprotecting and
  protecting that memory again.
- **Helper threads wake up at once on Windows.** Waking a sleeping game helper thread could take
  up to half a millisecond; it now takes about 6 microseconds. The frame rate didn't change
  measurably.
- **More detail in crash reports.** When the game's own code crashes, the report now shows the
  memory at the addresses it was using, whether the graphics card writes that memory, and what the
  emulator itself wrote there in the last few seconds.

## What's new (October 23, 2026, second build)

No shaders are compiled again after this update.

- **A smoother first lap.** In a race, the first lap of each session was the roughest, with
  freezes of up to a second, and laps got better from the second on. The emulator looked up each
  new pipeline (the compiled form of a draw's shaders and settings) in its cache on disk on the
  same thread that runs the game's graphics work, and on a track each lookup took 4 to 8 ms: about
  13 seconds of a 5-lap race at Tokyo against 11 AI cars, nearly all in lap 1. The lookups now run
  on 4 extra threads while the graphics work goes on, and in the same race lap 1 had two short
  stutters. Something new on screen can show up a frame or two late on lap 1 while its lookup
  finishes. `KYTY_PIPELINE_LOOKUP_THREADS=0` brings back the old way.

## What's new (October 23, 2026)

No shaders are compiled again after this update.

- **No more crash after many retries** ([issue 9](https://github.com/usa-ratt/KytyPS5-GT7/issues/9)). After about 10 minutes of
  Circuit Experience retries the emulator ended. Every slightly different texture sampler the
  game asked for (how a texture is filtered and wrapped) got a sampler of its own on the graphics
  card, and none was freed, until the driver's limit (4000 on NVIDIA) was reached. Samplers that
  come out the same are now shared, and the emulator stays under the limit: past it, the nearest
  sampler already made stands in.
- **No more crash on some depth and stencil settings** ([issue 8](https://github.com/usa-ratt/KytyPS5-GT7/issues/8)): a time trial
  on a new track ended after 16 minutes on a setting the PC graphics card cannot reproduce
  exactly. The emulator now draws the nearest setting it can, or leaves out that one draw, and
  writes which setting it was into the log.

## What's new (October 22, 2026, third build)

No shaders are compiled again after this update.

- **No more crash in Scapes** ([issue 11](https://github.com/usa-ratt/KytyPS5-GT7/issues/11)). After 10 to 15 minutes,
  as the next scene loaded, the emulator froze and closed without an error. The game gives some
  of its worker threads stacks inside its own graphics memory. The emulator watches parts of that
  memory for changes by making them read-only, and once a block holding one of those stacks was
  made read-only, the next write to the stack ended the emulator before it could report anything.
  The emulator now leaves the stacks of running threads alone. A 30-minute Scapes run no longer
  crashed.

## What's new (October 22, 2026, second build)

**All shaders are compiled again after this update** (pop-in the first time in each place, and a
few minutes for each car's first visit in ray tracing mode).

- **GT Sophy drives properly** ([issue 13](https://github.com/usa-ratt/KytyPS5-GT7/issues/13), [issue 14](https://github.com/usa-ratt/KytyPS5-GT7/issues/14)).
  Sophy's driving runs as a neural network on the graphics card. The emulator could not translate
  that one GPU program (it uses one instruction the emulator did not support yet), so the program
  was skipped and Sophy drove without it: hard into the wall the moment the countdown ended.
- **No more crash in time trials** on draws that are set up to output nothing
  ([issue 8](https://github.com/usa-ratt/KytyPS5-GT7/issues/8)). They are skipped now; they draw nothing on the console either.
- **No more crash in Circuit Experience (Eiger Nordwand, sector 3) and in Sophy races**
  ([issue 9](https://github.com/usa-ratt/KytyPS5-GT7/issues/9), [issue 13](https://github.com/usa-ratt/KytyPS5-GT7/issues/13)): a texture description with
  a speed hint value the emulator did not expect.
- **Red lights no longer show through walls in replays.** The game asks the graphics card how
  much of each light is visible (an occlusion query); the emulator now answers with the real
  count instead of "all of it", and reads the replays' depth picture the way the console does.
- **Large text is complete:** countdown numbers and result stamps lost everything past 128 pixels
  (fix from upstream KytyPS5, #1214, by Devran Cosmo Uenal).
- **Saved liveries get their picture.** The game encodes the livery's picture as JPEG and PNG,
  and the emulator had no JPEG encoder and handed the encoder stale pixels. Some parts of the
  picture can still have the wrong colour, and leaving the livery editor still shows error
  CE-210717 (see Known problems).
- **Contributed by Franseven:** the asynchronous compute rewind functions the game calls every
  frame, UI text blending, UI text sampled narrower than the target it was drawn into (the smeared
  text shadows), and ports of upstream fixes by nmzik (zero-stride buffer fetches, oversized image
  uploads, NOP marker tags, depth DCC metadata). Thank you!
- When the emulator closes because of a crash it could not report before (a stack overflow, a
  C++ exception, an abort), the log now ends with the reason and where it happened, and the
  `Perf:` lines show how much memory is in use.

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
- Tokyo: at some times of day and weather (twilight and night), the picture can turn green at
  some tunnel exits and under some overpasses, and fades back once past them. It depends on the
  camera angle, so watching the same replay twice can give green once and not the other time.
- Tokyo at night: some shadows on tunnel ceilings and walls move with the camera.
- **AMD RX 9070 XT:** races crash while they load, inside the AMD driver's shader compiler, on one
  compute shader ([issue 12](https://github.com/usa-ratt/KytyPS5-GT7/issues/12)). Workaround:
  start the emulator with `KYTY_SKIP_SHADER_HASHES=daced64c72398e80` (the steps are in issue 12).
  That shader does some of the game's calculations, so something in races may be a little off.
- **Scapes:** the controls stop working after Olivier offers the drift photography tutorial
  ([issue 19](https://github.com/usa-ratt/KytyPS5-GT7/issues/19)). The tutorial waits for the
  game's online servers, which the emulator has no stand-in for, so the Menu Book "Take a Photo of
  Your Car in Scapes" can't be finished yet. Closing the emulator is the only way out of it.
- **Livery editor:** leaving it shows error CE-210717 (the game uploads the livery to its online
  service, which the emulator has no stand-in for), and some parts of a saved livery's picture can
  have the wrong colour.
- A graphics driver crash, seen after many license test retries
  ([issue 3](https://github.com/usa-ratt/KytyPS5-GT7/issues/3)), is not fixed yet. If it happens
  to you, please add this build's crash report and the zipped log to that issue: the report now
  names the graphics buffer involved.
- In-game videos can play out of sync with their sound
  ([issue 4](https://github.com/usa-ratt/KytyPS5-GT7/issues/4)). Being looked into.
- **Intel Arc, GeForce GTX 10xx and Radeon RX 5000 cards:** since the October 26 build the
  emulator gets past start-up on cards without fragment shader barycentrics
  ([issue 20](https://github.com/usa-ratt/KytyPS5-GT7/issues/20)), but whether the game runs on
  them is not known yet. Please report how far it gets.
- **NVIDIA:** the driver's shader compiler can crash while it compiles new pipelines
  ([issue 18](https://github.com/usa-ratt/KytyPS5-GT7/issues/18)). Being looked into; the crash
  report names the pipeline.
- If a loading screen still never ends, please open an issue with the log.

## Credits

- **Franseven** contributed fixes to this branch: the asynchronous compute rewind functions the game
  calls every frame, UI text blending (color One, alpha Zero over a source-alpha destination, with
  dead render-target exports no longer blocking it), UI text sampled narrower than the target it was
  drawn into (the text shadow smear), and ports of upstream fixes by nmzik (zero-stride buffer
  fetches, oversized image uploads, NOP marker tags, depth DCC metadata). His second batch ported
  mainline changes by nmzik (PM4 barriers, alpha-to-coverage, empty indirect dispatches, the
  tiler's pipelines, audio system state, HTTP functions), Mr. Big Britches (the shared protection
  lock, AudioOut2 context mutexes), NeySlim (lock-free dirty bits) and Arda Sahin (image usage),
  plus his own fixes for storage buffer ranges, read-only buffer syncs and ATRAC9 configurations.

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
