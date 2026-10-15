#ifndef KYTY_COMMON_HOSTSYMBOLS_H_
#define KYTY_COMMON_HOSTSYMBOLS_H_

#include <cstdint>
#include <string>

namespace Common {

// The function of the emulator (or of a system library) that holds `address`, named from the
// PDB next to the executable, as the host stacks of fatal errors are (HostBacktrace); empty when
// no symbol covers it (the game's code). `start`, when given, gets the function's first address.
// Windows only; empty elsewhere.
[[nodiscard]] std::string HostSymbol(uint64_t address, uint64_t* start = nullptr);

} // namespace Common

#endif // KYTY_COMMON_HOSTSYMBOLS_H_
