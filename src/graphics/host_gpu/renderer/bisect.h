#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_

#include <cstdint>
#include <string>

// Diagnostics: KYTY_BISECT="<step>;<step>;...". Each F8 press saves a picture of the frame as it
// is (image-dumps/bisect_<press>_<step>.jpg, next to the log) and then turns on the next step,
// only that one; after the last step it goes back to normal. So one scene can be compared with
// each part of the frame changed in turn: hold still, press F8, wait a second, press again, and
// so on. The step that is on is named at the top of the screen, and each press logs a "Bisect:"
// line. KYTY_BISECT_START=<n> turns step n on from the start, for what the game makes while it
// loads. A step is one action or several joined with '+', after an optional "<name>=":
//   skip:<hash>  the draws and dispatches with a shader of that hash (16 hex digits, or the first
//                8 or more of them) are not run
//   lit:big      depth comparisons against depth images 4096 or more texels wide (the top-down
//                shadow maps of a whole track) pass, as if nothing cast a shadow there
//   lit:all      every depth comparison passes: no shadow map darkens anything
//   mips:cs      a compute shader's sampler with unnormalized coordinates gets the T#'s whole view,
//                as before 2026-10-15 (round 56), instead of its first mip. Graphics stages keep
//                the whole view anyway: mips:whole is the same as mips:cs, and mips:graphics
//                changes nothing
// and, for A/B tests of performance with the Perf lines (common/perfStats.h), which name the step:
//   cpu:vcache   the process runs on the processors sharing the largest L3 cache (cpu:frequency
//                the others, cpu:all every one); without it, as at the start (KYTY_CPU_SET)
//   readback:<n> a game thread's fault on GPU-written memory downloads a window of n KB around
//                it (KYTY_READBACK_WINDOW_KB, default 4096; 0 the faulting pages only)
//   log:off      LOGF writes nothing
//   writes:off   a store to a page protected because the GPU wrote to it downloads the page first,
//                as before 2026-10-16 (round 61), instead of being done in place
//                (KYTY_NO_CLEAN_WRITE_EMULATION=1 for the whole run)
//   gputime:<n>  the next n frames are timed on the host GPU (GpuTiming, "GpuTiming frame" lines),
//                once, when the step turns on
// and, for the stalls (renderer/stallWatch.h):
//   nancheck:off the NaN check (KYTY_NAN_CHECK) checks and scrubs nothing
//   present:fence the presenter waits on the CPU for the swapchain image it acquires, and its
//                command buffer waits for nothing on the GPU (KYTY_PRESENT_ACQUIRE=fence for the
//                whole run), instead of making the GPU queue wait for the image
//   gc:off       neither the buffer cache nor the texture cache collects anything
//   gpuwatch     every draw and dispatch is timed on the host GPU, and a command buffer that
//                takes KYTY_GPU_WATCH_MS (default 30) or more is logged with its costliest ones
//                ("GpuTiming slow" lines; KYTY_GPU_WATCH=1 for the whole run)
// and, for what holds the frame rate (round 63):
//   tess:off     tessellated draws (patch primitives) are not run
//   tess:max:<f> the tessellator draws factors up to f at most: a larger VGT_HOS_MAX_TESS_LEVEL
//                (or none) is lowered to f, so the tessellation shaders are made again for it on
//                their next draws
//   tess:bit:<k> the tessellated draws whose number in their frame (0 for the first) has bit k
//                set are not run (round 82): with a step for each bit, the pictures in which a
//                picture fault is gone spell the number of the draw that makes it. The first
//                whole frame of each step lists its tessellated draws ("Bisect: tess draw" lines).
//   flush:<n>    a command buffer goes to the GPU after n draws (KYTY_DRAW_FLUSH_INTERVAL for the
//                whole run, default 16)
//   readahead    what game threads read back is downloaded as soon as the GPU writes it again,
//                so their reads find it in memory (KYTY_READAHEAD=1 for the whole run)
// and (round 64):
//   cmask:off    a multisampled color target's CMASK fast clears are left out, as before round
//                63b (KYTY_MSAA_CMASK_CLEARS=0 for the whole run)
//   fastread:off a game thread's read of bytes the GPU has finished writing waits for a download
//                behind the GPU's queue, as before round 64, instead of one on the readback queue
//                (KYTY_FAST_READBACK=0 for the whole run)
//   fastread:any the fast readback copies bytes even while a command that stores through shader
//                addresses is unfinished (it may miss what that command stores there: a test of
//                what waiting for those commands costs)
//   fastread:gpu the GPU thread waits for a fast readback's copy and puts its bytes in guest
//                memory, as before round 152, instead of the reading thread
//                (KYTY_FAST_READBACK=gpu for the whole run)
//   cuts156:off  as before round 156: the GPU thread waits for the page protection worker
//                rather than doing a batch it has not started, and every texture description is
//                looked up in its map (no direct-mapped front)
//   liftwait:old a game thread's read of pages a download is still bringing in waits for the
//                latest download of all, as before round 153, instead of the ones that cover its
//                pages (KYTY_LIFT_WAIT=old for the whole run)
// and (round 65):
//   recheck      a render target's fast clear is checked on the GPU again at the first draw of
//                every command buffer, as before round 65, even when nothing wrote its metadata
//                since the last check (KYTY_GPU_CLEAR_RECHECK=1 for the whole run)
//   locks:spin   the texture cache's and the memory tracker's locks spin as before round 65
//                (a test-and-set loop) instead of waiting politely (KYTY_LOCKS_SPIN=1 for the
//                whole run)
//   htile:depth  a depth target's HTile fill clears its depth alone, as before round 65, even
//                when the fill's value clears its stencil too (KYTY_HTILE_STENCIL_CLEARS=0 for
//                the whole run)
// and (round 66):
//   patternfill:off the SDK's pattern fill shader runs as a dispatch, as before round 66, instead
//                of clearing the image (a stencil plane, a target) whose memory it fills
//                (KYTY_PATTERN_FILLS=0 for the whole run)
//   faultblock:off a CPU write fault releases its own page from write tracking, as before round
//                66, even in a block that faults again (KYTY_FAULT_BLOCK_KB=0 for the whole run)
//   faultblock:<kb> the block such a fault releases is <kb> KB (8 to 1024, a power of two)
//                instead of KYTY_FAULT_BLOCK_KB's (default 64; round 155)
// and (round 67):
//   popwait      the deferred operations a draw or a dispatch runs wait for the priority
//                callbacks of their ticks, as before round 67, instead of being left for a later
//                draw while those run (KYTY_POP_WAIT=1 for the whole run)
//   texmemo:off  every texture binding is looked up in the texture cache again, as before round
//                67, instead of reusing one the same command buffer resolved for the same
//                descriptor (KYTY_TEXTURE_MEMO=0 for the whole run)
// and (round 68):
//   rebind2      a dispatch makes its image and buffer bindings twice, as before round 68
//   drawprep:off the draw-prep scanner prepares nothing and the GPU thread walks every draw's and
//                dispatch's descriptors itself (KYTY_DRAW_PREP=0 for the whole run)
//   gpuwrites:fault the GPU thread's own stores to guest memory (labels, WRITE_DATA) fault on
//                pages protected for tracking, as before round 68, instead of lifting the
//                protection first
// and (round 69):
//   bdawalk:all  a dispatch or draw that reads memory through shader addresses synchronizes every
//                cached buffer of every tracker region holding a CPU-dirty page, as before round
//                69, instead of the regions where something changed since the last walk
//   checkpoints:on every draw, dispatch and transfer records an NVIDIA diagnostic checkpoint, as
//                before round 69 (KYTY_GPU_CHECKPOINTS=1 for the whole run)
//   speccheck:all every draw and dispatch checks its specialization reads against what it
//                writes, as before round 69, instead of one in 64 (KYTY_SPEC_READ_CHECK=1 for the
//                whole run)
//   dflush:<n>   a command buffer goes to the GPU after n dispatches (KYTY_DISPATCH_FLUSH_INTERVAL
//                for the whole run, default 16); dflush:0 only at the usual points, as before
//                round 69
// and (round 70):
//   drawprep:taint the draw-prep scanner prepares nothing more up to the next suspend point once
//                it meets register pairs it cannot read (the GPU wrote them), as before round 70
//   bdaspan:off  the BDA walk synchronizes each CPU-dirty run of pages on its own, as in round 69,
//                instead of each cached buffer once over the span of its CPU-dirty runs
// and (round 71):
//   scanplan:shared the draw-prep scanner walks the program's own resource plan under its lock,
//                as before round 71, instead of a copy of its own (the GPU thread then waits
//                whenever the scanner is walking a program it needs)
//   views:old    a texture or target found inside a larger cached image is viewed from the layer
//                where its memory starts, as before round 71, dropping its descriptor's first
//                layer (BASE_ARRAY / SLICE_START); and placed where the last overlap left it, as
//                before round 74 (TextureCache::FindImage)
//   grids:off    a pixel shader's light grids (two-channel uint array textures of up to 256 KB)
//                are bound as null textures: every light count reads as zero
// and (round 72):
//   grids:raw    a light grid's cells that ask for more than 1024 lights are uploaded as they are,
//                as before round 72, instead of as empty cells (they are still counted and
//                logged: the "LightGrid:" lines)
// and (round 73):
//   scan:one     the draw-prep scanner walks every draw on its own thread, as before round 73,
//                instead of on its workers (KYTY_DRAW_PREP_WORKERS=0 for the whole run)
// and (round 74):
//   hotpages     pages the CPU writes again within 250 ms, three times running, stay writable
//                instead of being write-protected after each upload (RenderContext::HandleFault);
//                they are uploaded again at each new slice and each wait on memory, written or
//                not (KYTY_HOT_PAGES=1 for the whole run)
// and (round 75):
//   depth:equal-relaxed[:<units>]  a draw that tests depth for EQUAL (GT7's G-buffer pass of
//                cars, after their depth prepass) tests it for "as close or closer" instead,
//                biased towards the camera by <units> (default 256) of the primitive's depth
//                precision and by its depth slope, without writing depth: a pixel whose depth the
//                two passes compute that close is drawn too (SetGraphicsDynamicParams)
// and (round 76):
//   hiz:off      a hull shader's texture sampled through a sampler with unnormalized coordinates
//                (GT7's depth pyramid, which culls the patches of car bodies another object hides)
//                is bound as a null texture: no patch is culled that way
//   unorm:lod    a sampler with unnormalized coordinates keeps the guest's LOD range instead of
//                0 to 0 (KYTY_UNNORMALIZED_LOD=1 for the whole run): an explicit LOD past 0 then
//                reaches the mip it names, as on the console, where Vulkan's rule for such a
//                sampler (one mip, LOD 0) clamps it to the first
// and (round 77):
//   unorm:old    a hull shader samples through a sampler with unnormalized coordinates as before
//                round 77 (KYTY_UNNORMALIZED_FIX=0 for the whole run), instead of running its copy
//                that turns them into normalized ones at the LOD it names
//                (SpirvNormalizedExplicitLod)
// and (round 78):
//   drawprep:notess the draw-prep scanner leaves tessellated draws to the GPU thread, as before
//                round 78, which then walks their four stages' descriptors itself
//   texmemo:global a texture binding kept for reuse is looked up again whenever any image was
//                made or dropped anywhere, as before round 78, instead of only when one was over
//                memory near its own (TextureCache::RangeStamp)
// and (round 79):
//   faultblock:old a write fault in a block an image lies in releases its own page only, as
//                before round 79, instead of the block's pages no image lies on as well
//   alias:old    a texture read of memory another image of another kind holds replaces that image
//                (or is copied into a larger one), as before round 79, even when neither was
//                ever written by the GPU (TextureCache::KeepReadOnlyAlias)
//   spechash:off a stage's shader permutation is found by comparing its specialization, as
//                before round 79, instead of by the hash the draw-prep scanner made of it
//   cpu:gpuvcache the GPU thread and the emulator's threads that feed it (draw preparation,
//                Vulkan recording) prefer the processors sharing the largest L3 cache and every
//                other thread the rest (cpu:gpufrequency the other way round)
// and (round 80):
//   cpu:all      every thread runs on any processor, as before round 80 (which places the GPU
//                thread and its helpers as cpu:gpuvcache does on a Ryzen with a 3D V-Cache die
//                beside another, Common::StartCpuSet)
//   dirtyindex:off whether guest bytes hold GPU-written ones is asked of the range tree every
//                time, as before round 80, instead of first of a bitmap of the pages they touch
//   flipcheck:old a flip reads the display buffer's compression metadata back for its fast
//                clear, waiting for everything queued on the GPU, as before round 80, instead of
//                checking it on the GPU as a render target's is (or, where that cannot be,
//                copying it on the readback queue once its writer is done)
//   bdaprotect:old a walk for shaders that read memory through addresses write-protects again
//                the pages it uploaded buffer by buffer, as before round 80, instead of all those
//                of a tracker region at once
//   listreset:off an indexed list draw whose indices hold the guest's primitive reset index draws
//                it as a vertex, as before round 80, instead of dropping the primitive it falls in
//                (KYTY_LIST_RESET=0 for the whole run)
// and, round 82 (the A/B of its GPU thread and fault changes):
//   scanwave:old  the draw-prep scanner prepares dispatches with the default wave size, as before
//                (a wave32 dispatch then finds nothing prepared)
//   drawstate:old each draw value-initializes a whole draw state (36 KB) on the stack, instead of
//                reusing one and clearing the tessellation stages only after a tessellated draw
//   refresh:old   every draw and dispatch reads the GPU's timeline semaphore, instead of every
//                eighth one when nothing waits on it
//   expand:old    a compute shader known to make no calls is still run through the function
//                expander (which compares its whole code) at every dispatch
//   backing:locked copies to and from guest memory hold the backing store's lock while they copy,
//                as before (game threads' reads waited for the GPU thread's uploads)
// and, round 84:
//   buflen:old    a read-only buffer binding's length, which the shader's bounds checks compare
//                with, is rounded down to whole dwords, as before, instead of up (byte reads of
//                the last bytes of a range that ends inside a dword read zero: the roof spike)
//   cpu:gpucore   as cpu:gpuvcache (the placement a run starts with on a Ryzen X3D), with the GPU
//                thread alone on one core of the V-Cache die, whose SMT sibling no thread of the
//                process is given (Common::SetProcessCpuSet)
// and, round 85:
//   cube:<n>      the draws into a reflection cube (a color target of six square layers) run every
//                nth frame only, n from 1 to 8 (KYTY_CUBE_EVERY for the whole run)
// and, round 90:
//   bdasync:submission the CPU's writes are synchronized for shaders that read memory through
//                addresses once a guest submission, as before round 90, instead of again after
//                each wait on memory that passes in it (KYTY_BDA_WAIT_SYNC=0 for the whole run)
// and, round 91:
//   gbuffer:sync a 3D pass into a texture (color targets of 256 texels or more each way, a depth
//                test that writes depth) compiles its new pipelines on the GPU thread, as before
//                round 91 when the target was small and not drawn every frame, instead of being
//                skipped while they compile (KYTY_DEFER_GBUFFER=0 for the whole run)
//   depthread:old a draw that samples its depth target read-only binds it for the attachment
//                alone first, as before round 91 (two barriers per draw), instead of for the
//                attachment and a shader read at once
// and, round 92 (from BryanKAdams' BryKytyPS5):
//   stream:host  the per-draw copies of small CPU-written buffers, flattened resource tables and
//                shader data go to a ring in cached host memory instead of the one in device
//                memory (write-combined), as BryKytyPS5 does (KYTY_STREAM_HOST=1 for the whole run;
//                a race in round 92: 0.8 ms a frame slower, so not the default)
//   buffers:dedicated a game buffer made while the step is on gets memory of its own, as before,
//                instead of a place in a shared block when it is 64 MB or less
//                (KYTY_BUFFERS_DEDICATED=1 for the whole run)
// and, round 111:
//   memsync:async the read-only protections the GPU thread's buffer syncs ask for, and their
//                uploads' reads of guest memory, are done on a worker thread (PageManager's
//                "GpuProtect"), which the GPU thread waits for before it stores to guest memory
//                (KYTY_ASYNC_MEMORY_SYNC=1 for the whole run)
// and, round 114 (slowing one part down on purpose: how much the frame slows shows how much that
// part holds the frame rate, and so how much making it faster would give):
//   slow:gpu[:<us>]    the GPU thread spends <us> microseconds more (3 by default) on each draw
//                and each dispatch, busy
//   slow:faults[:<us>] a game thread spends <us> microseconds more (15 by default) on each fault
//                on memory the GPU uses, busy
//   slow:reads[:<us>]  a game thread's read of bytes the GPU wrote, which waits for them, waits
//                <us> microseconds more (2500 by default)
// and, round 130:
//   probe:sync   a new pipeline is looked up in the driver's caches on the GPU thread, as before
//                (KYTY_PIPELINE_LOOKUP_THREADS=0 for the whole run), not on a worker thread
// and, round 131:
//   phases:all   the GPU phases time the draws and dispatches of every frame, as before
//                (KYTY_GPU_PHASES_EVERY=1 for the whole run), not of one frame in four
// and, round 132 (each the way before round 132, for an A/B against the new defaults):
//   memsync:sync  the GPU thread's syncs make their read-only protections and their uploads'
//                reads themselves (KYTY_ASYNC_MEMORY_SYNC=0 for the whole run)
//   syncwait:old  the game's waits on an address use the condition variable, with no spin
//                (KYTY_SYNC_WAIT=old for the whole run)
//   flush:16     a command buffer is submitted every 16 draws (64 now; KYTY_DRAW_FLUSH_INTERVAL)
//   storeclean:off a GPU-thread store to watched CPU-clean pages makes them CPU-dirty
//                (KYTY_STORE_CLEAN=0 for the whole run; BufferCache::WriteClean)
// and, round 133:
//   hold:off     every frame is shown, also one that left out many draws while their pipelines
//                were looked up (KYTY_HOLD_SKIPPED_DRAWS=0 for the whole run;
//                GpuPhases::TakeFrameHold)
// and, round 135 (BufferCache::ReadMemoryStep):
//   cleanpage:old  a game thread's fault on a page another thread's read has just made current
//                downloads the rest of its window and waits for it, as before (KYTY_CLEAN_PAGE=old
//                for the whole run)
//   readwait:writer a game thread's read of bytes whose writer the GPU is still running waits for
//                that command buffer and then copies them on the readback queue, instead of a
//                download behind every command buffer recorded since (KYTY_READ_WAIT=writer for
//                the whole run)
//   barriers:global the PM4 cache flushes, RELEASE_MEM and suspend points record a full barrier,
//                as before round 135 (KYTY_PM4_BARRIERS=global for the whole run)
//   protect:exclusive the page tracker's protections take the address space's lock exclusively,
//                as before round 135 (KYTY_PROTECT_LOCK=exclusive for the whole run)
// KYTY_BISECT_AB=1: while a step is on, it is turned off and on again at each Perf line (every
// 2 s), so its lines and the lines without it ("<name> base") alternate, and each can be compared
// with the ones next to it, made on nearly the same frames.
namespace Libs::Graphics::Bisect {

// The GPU thread, at its start: applies the start step's CPU set and log setting.
void Start();

// F8 (window thread): asks for a picture of the frame as it is and for the next step after it;
// returns the line to show for that step.
std::string Advance();

// Presenter thread: the file name (no extension) of the picture Advance asked for, or empty.
// Once the picture is recorded (or there was no frame to take), ApplyPending turns on the step.
[[nodiscard]] std::string TakePictureRequest();
void                      ApplyPending();

// Round 89: a picture of the next presented frame (into image-dumps, as F8's), with no step
// change: F9 asks for one with its frame capture, so the capture comes with what the screen
// showed. Any thread.
void RequestPicture(const std::string& name);

// GPU thread: what the step that is on asks for.
[[nodiscard]] bool Active();
// GPU thread: whether some step has tess:bit; that bit of the step that is on (-1 for none); the
// number of the step that is on (0: normal, or KYTY_BISECT_AB's base lines).
[[nodiscard]] bool     TessBitsConfigured();
[[nodiscard]] int32_t  TessSkipBit();
[[nodiscard]] uint32_t StepNumber();
[[nodiscard]] bool SkipsShader(uint64_t hash);
[[nodiscard]] bool PassesDepthCompare(bool big_depth_image);
// Whether a compute shader's sampler with unnormalized coordinates keeps the T#'s whole view.
[[nodiscard]] bool WholeUnnormalizedViews();
// The readback window the step that is on asks for, in KB, or -1.
[[nodiscard]] int64_t ReadbackWindowKB();
// Any thread: whether the step that is on turns off stores done in place (writes:off).
[[nodiscard]] bool CleanWritesOff();
// Any thread: nancheck:off, present:fence, gc:off.
[[nodiscard]] bool NanCheckOff();
[[nodiscard]] bool PresentFence();
[[nodiscard]] bool GcOff();
// Any thread: tess:off; tess:max's factor (0 for none); flush:'s draws (0 for the usual);
// readahead.
[[nodiscard]] bool     TessOff();
[[nodiscard]] float    TessMaxLevel();
[[nodiscard]] uint32_t FlushInterval();
[[nodiscard]] bool     Readahead();
// Any thread: cmask:off; fastread:off; fastread:any; fastread:gpu.
[[nodiscard]] bool MsaaCmaskOff();
[[nodiscard]] bool FastReadbackOff();
[[nodiscard]] bool FastReadbackAnyAddress();
[[nodiscard]] bool FastReadbackOnGpuThread();
[[nodiscard]] bool LiftWaitOld();
[[nodiscard]] bool Cuts156Off();
// Any thread: recheck; locks:spin; htile:depth; patternfill:off; faultblock:off.
[[nodiscard]] bool GpuClearRecheck();
[[nodiscard]] bool LocksSpin();
[[nodiscard]] bool HtileDepthOnly();
[[nodiscard]] bool PatternFillsOff();
[[nodiscard]] bool FaultBlocksOff();
// Any thread: faultblock:<kb>'s block size in KB, or -1 (round 155).
[[nodiscard]] int32_t FaultBlockKB();
// Any thread: popwait; texmemo:off.
[[nodiscard]] bool PopWait();
[[nodiscard]] bool TextureMemoOff();
// Any thread: rebind2; drawprep:off; gpuwrites:fault.
[[nodiscard]] bool RebindTwice();
[[nodiscard]] bool DrawPrepOff();
[[nodiscard]] bool GpuWritesFault();
// Any thread: bdawalk:all; checkpoints:on; speccheck:all.
[[nodiscard]] bool BdaWalkAll();
[[nodiscard]] bool CheckpointsOn();
[[nodiscard]] bool SpecCheckAll();
// Any thread: dflush:'s dispatches (-1 for the usual).
[[nodiscard]] int32_t DispatchFlushInterval();
// Any thread: drawprep:taint; bdaspan:off.
[[nodiscard]] bool DrawPrepTaint();
[[nodiscard]] bool BdaSpanOff();
// Any thread: scanplan:shared; views:old; grids:off; grids:raw.
[[nodiscard]] bool ScanPlanShared();
[[nodiscard]] bool OldViewBase();
[[nodiscard]] bool LightGridsOff();
[[nodiscard]] bool LightGridsRaw();
// Any thread: scan:one.
[[nodiscard]] bool ScanOneThread();
// Any thread: hotpages, or KYTY_HOT_PAGES=1.
[[nodiscard]] bool HotPages();
// Round 111, any thread: the read-only protections the GPU thread's buffer syncs ask for and
// their uploads' reads are done on the page manager's worker (PageManager::DeferScope,
// PostDeferred): by default since round 132; not with KYTY_ASYNC_MEMORY_SYNC=0 or in a
// memsync:sync step; always in a memsync:async step.
[[nodiscard]] bool AsyncMemorySync();
// Any thread: hiz:off.
[[nodiscard]] bool HiZOff();
// Any thread: unorm:lod.
[[nodiscard]] bool UnnormalizedLod();
// Any thread: unorm:old.
[[nodiscard]] bool UnnormalizedOld();
// Any thread: drawprep:notess; texmemo:global.
[[nodiscard]] bool DrawPrepNoTess();
[[nodiscard]] bool TextureMemoGlobal();
// Any thread: faultblock:old; alias:old; spechash:off.
[[nodiscard]] bool FaultBlockAroundImagesOff();
[[nodiscard]] bool ReadOnlyAliasesOff();
[[nodiscard]] bool SpecHashOff();
// Any thread: flipcheck:old; bdaprotect:old; listreset:off (dirtyindex:off is applied to
// PagedRangeSet directly).
[[nodiscard]] bool FlipCheckOld();
[[nodiscard]] bool BdaProtectOld();
// Round 108: yield:off, graphics streams do not stop for compute submissions
// (GuestGpu::ComputeYield).
[[nodiscard]] bool ComputeYieldOff();
// Round 109: yield:<microseconds>, the time a graphics stream runs before it may stop for one
// (0 never stops); -1 when the step does not set it.
[[nodiscard]] int64_t ComputeYieldMicroseconds();
[[nodiscard]] bool ListResetOff();
// Any thread: scanwave:old; drawstate:old; refresh:old; expand:old (backing:locked is applied to
// the kernel's memory, LibKernel::Memory::SetBackingCopyLocked).
[[nodiscard]] bool ScanWaveOld();
[[nodiscard]] bool DrawStateOld();
[[nodiscard]] bool RefreshOld();
[[nodiscard]] bool ExpandOld();
// Any thread: buflen:old.
[[nodiscard]] bool BufferLengthOld();
// Any thread: cube:<n>'s n; 0 when no step sets it.
[[nodiscard]] uint32_t CubeEvery();
// Any thread: depth:equal-relaxed's bias in units of depth precision; 0 when off.
[[nodiscard]] float DepthEqualRelaxed();
// GPU thread: bdasync:submission.
[[nodiscard]] bool BdaSyncSubmission();
// GPU thread: probe:sync (round 130); phases:all (round 131).
[[nodiscard]] bool PipelineProbeSync();
[[nodiscard]] bool PhaseTimingAll();
// GPU thread: storeclean:off (round 132).
[[nodiscard]] bool StoreCleanOff();
// GPU thread: hold:off (round 133).
[[nodiscard]] bool FrameHoldOff();
// GPU thread: cleanpage:old, readwait:writer, barriers:global (round 135; protect:exclusive is
// applied to the kernel's memory, LibKernel::Memory::SetProtectLockExclusive).
[[nodiscard]] bool CleanPageOld();
[[nodiscard]] bool ReadWaitWriter();
[[nodiscard]] bool GlobalPm4Barriers();
// GPU thread: gbuffer:sync; depthread:old (round 91).
[[nodiscard]] bool GbufferSync();
[[nodiscard]] bool DepthReadOld();
// Round 92: some step has stream:host (the host ring is made at the start for it); the current
// one has it; the current one has buffers:dedicated.
[[nodiscard]] bool StreamHostConfigured();
[[nodiscard]] bool StreamHost();
[[nodiscard]] bool BuffersDedicated();
// Round 114, any thread: slow:gpu's, slow:faults' and slow:reads' microseconds (0 when the
// current step has none), and the busy wait they ask for.
[[nodiscard]] uint32_t SlowGpuMicroseconds();
[[nodiscard]] uint32_t SlowFaultMicroseconds();
[[nodiscard]] uint32_t SlowReadMicroseconds();
void                   SpinMicroseconds(uint32_t microseconds);
// Round 115, any thread: KYTY_ZERO_NEW_MEMORY=1. The memory of a new game buffer is zeroed before
// what it takes over from older buffers is copied in, and a new image nothing uploads into (a
// multisampled or compressed one) is cleared to zero, instead of both starting with whatever the
// graphics driver's memory held (which differs from card to card).
[[nodiscard]] bool ZeroNewMemory();

} // namespace Libs::Graphics::Bisect

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_
