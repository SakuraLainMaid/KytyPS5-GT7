#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstdint>
#include <string>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);

// Reads [vaddr, vaddr + size) for a load that faulted at fault_vaddr; false to decline.
using LoadReader = bool (*)(uint64_t fault_vaddr, uint64_t vaddr, void* data, uint64_t size);
// Completes a faulting load whose memory operand covers fault_vaddr by reading the operand through
// `read` instead of the faulting mapping, then steps past the instruction. Pure loads only: MOV,
// MOVZX, MOVSX, MOVSXD, CMP and TEST with a memory operand, and SSE/AVX moves from memory into a
// vector register. False, with the context untouched, for anything else or when `read` declines.
[[nodiscard]] bool TryEmulateLoad(void* native_context, uint64_t fault_vaddr, LoadReader read);

// Writes [vaddr, vaddr + size) for the store at `pc` that faulted at fault_vaddr; false to decline.
using StoreWriter = bool (*)(uint64_t pc, uint64_t fault_vaddr, uint64_t vaddr, const void* data,
                             uint64_t size);
// Completes a faulting store whose memory operand covers fault_vaddr by handing its bytes to
// `write` instead of the faulting mapping, then steps past the instruction. Plain stores only: MOV
// and MOVNTI from a general-purpose register (or MOV from an immediate), and SSE/AVX moves from a
// vector register to memory. False, with the context untouched, for anything else (read-modify-
// write and string instructions among them) or when `write` declines. Windows only.
[[nodiscard]] bool TryEmulateStore(void* native_context, uint64_t fault_vaddr, StoreWriter write);

// Diagnostics: the instruction at `address` in Intel syntax, or empty when it does not decode.
// The bytes must be readable. Windows only (empty elsewhere).
[[nodiscard]] std::string DescribeInstruction(uint64_t address);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
