#ifndef KYTY_COMMON_LASTWORDS_H_
#define KYTY_COMMON_LASTWORDS_H_

#include <cstdint>

namespace Common::LastWords {

// Round 125: what the emulator writes to the console and the log, flushed, when the process ends
// in a way the fault handler does not report: an exception nothing handled (a stack overflow, a
// C++ exception such as a failed allocation, a division by zero), std::terminate, abort(), an
// invalid argument to a C runtime function or a pure virtual call. Before, such an end closed the
// window with nothing in the log (the last lines still in its buffer). The report is written by
// a thread of its own (the failing thread's stack may be exhausted), with the failing thread's
// host stack. Windows; elsewhere nothing is installed.
void Install();

// Run once, on the failing thread, before the final report of a process that is ending: for
// what must not outlive it (the emulated wheel stops its forces).
void SetFinalAction(void (*action)());

// From the host fault handler (first chance), with the fault's registers (a CONTEXT): an
// exception no handler can take on this thread is reported now, as the process will not reach
// the unhandled-exception filter (a thread running on a guest stack, whose stack bounds are
// cleared); so is a stack overflow or a division by zero on the host's own stacks (the first
// four). Windows only.
void ReportFirstChance(uint32_t code, uint64_t address, const uint64_t* parameters,
                       uint32_t parameter_count, const void* context);

} // namespace Common::LastWords

#endif // KYTY_COMMON_LASTWORDS_H_
