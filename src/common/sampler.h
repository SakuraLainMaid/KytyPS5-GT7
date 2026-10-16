#ifndef KYTY_COMMON_SAMPLER_H_
#define KYTY_COMMON_SAMPLER_H_

// Diagnostics: KYTY_SAMPLE=<name>[,<name>...] samples the threads with those names (a name ending
// in '*' takes every thread whose name starts with the rest, as Job#*; KYTY_SAMPLE=1 takes
// Thread_Gpu, Job#0, Job#1, GpuRecord, Thread_DrawPrep and GpuPriority) KYTY_SAMPLE_HZ times a
// second (default 1000). Every KYTY_SAMPLE_REPORT seconds (default 10) it logs, for each:
//   - "self": the functions its samples fell in;
//   - "total": the emulator's functions on its stack (each once a sample), the stack walked with
//     the Windows unwind data on a copy taken while the thread was suspended;
//   - "paths": the leaf and the five frames below it, joined by "<" ("called from");
//   - "waits": the samples whose leaf waits in the kernel (a lock, a condition variable, an event,
//     a fence, a sleep), by the emulator's first four frames below the wait;
//   - "lines": the source lines its samples fell on in the emulator's busiest functions;
//   - "depth": how deep the walks went.
// Functions are named from kyty_emulator.pdb next to the exe; code without symbols (the game's) is
// named by address, in 256-byte steps, and the game's hottest code is listed with its bytes
// ("Sample <thread> guest"). Each sample suspends the thread for a few microseconds. Windows only.
namespace Common::Sampler {

// Starts the sampler when KYTY_SAMPLE asks for it (the first call; later ones do nothing).
void Start();

} // namespace Common::Sampler

#endif // KYTY_COMMON_SAMPLER_H_
