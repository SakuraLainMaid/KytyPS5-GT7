#ifndef KYTY_COMMON_SAMPLER_H_
#define KYTY_COMMON_SAMPLER_H_

// Diagnostics: KYTY_SAMPLE=<name>[,<name>...] samples the threads with those names (a name ending
// in '*' takes every thread whose name starts with the rest, as Job#*; KYTY_SAMPLE=1 takes
// Thread_Gpu, Job#0, GpuRecord and Thread_DrawPrep) KYTY_SAMPLE_HZ times a second (default 1000).
// Every KYTY_SAMPLE_REPORT seconds (default 10) it logs, for each, the functions its samples fell
// in ("Sample <thread> self:") and the emulator's functions on its stack, followed through the
// frame-pointer chain ("Sample <thread> total:"). Functions are named from kyty_emulator.pdb next
// to the exe; code without symbols (the game's) is named by address, in 256-byte steps. Each
// sample suspends the thread for a few microseconds. Windows only.
namespace Common::Sampler {

// Starts the sampler when KYTY_SAMPLE asks for it (the first call; later ones do nothing).
void Start();

} // namespace Common::Sampler

#endif // KYTY_COMMON_SAMPLER_H_
