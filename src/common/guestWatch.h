#ifndef EMULATOR_SRC_COMMON_GUESTWATCH_H_
#define EMULATOR_SRC_COMMON_GUESTWATCH_H_

#include <cstdint>

// Round 121, diagnostics (Windows only): hardware write watches on guest memory. Up to four
// addresses (debug registers DR0 to DR3, 8 bytes each) are watched on every thread that exists
// when they are armed. Each write logs one "GuestWatch:" line: the watch, the value now there,
// the code address after the store (relative to the main program when it lies in it), the
// general registers, and the main program's return addresses found on the stack. The first hit at
// each code address (eight of them) also logs the 512 code bytes from 0x180 before it ("GuestWatch
// code <address> +<offset>:", 32 a line), to disassemble. At most 48 hits are logged; then a count
// every 1000 hits.
namespace Common::GuestWatch {

// Once the main program is loaded: its image, for relative code addresses.
void SetProgram(uint64_t base, uint64_t size);

// Arms watches on these addresses (8-byte aligned, at most 4; a second call adds to the first,
// up to 4 in all). Any thread; the calling thread itself is not watched. `reason` goes to the log.
// Returns how many threads were armed.
uint32_t Arm(const uint64_t* addresses, uint32_t count, const char* reason);

} // namespace Common::GuestWatch

#endif // EMULATOR_SRC_COMMON_GUESTWATCH_H_
