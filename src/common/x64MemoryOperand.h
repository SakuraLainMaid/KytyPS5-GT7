#ifndef KYTY_COMMON_X64MEMORYOPERAND_H_
#define KYTY_COMMON_X64MEMORYOPERAND_H_

#include <array>
#include <cstdint>
#include <cstring>

// Round 107, diagnostics (the sampler's "loads" lines): the address an x86-64 instruction's memory
// operand names, from its bytes and the registers before it runs: [base + index * scale + disp]
// through general registers. Not for RIP-relative operands, FS/GS segments, LEA (it reads nothing),
// or instructions without a ModRM byte. A thread suspended where it spins shows the address it
// polls this way.
namespace Common::X64 {

// rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8 .. r15, as ModRM and SIB number them.
using Registers = std::array<uint64_t, 16>;

namespace Detail {

[[nodiscard]] constexpr bool OneByteHasModRm(uint8_t op) {
	if (op < 0x40) {
		return (op & 0x07u) < 0x04u; // the ALU group's r/m forms (add .. cmp)
	}
	return op == 0x63 || op == 0x69 || op == 0x6b || (op >= 0x80 && op <= 0x8f) || op == 0xc0 ||
	       op == 0xc1 || op == 0xc6 || op == 0xc7 || (op >= 0xd0 && op <= 0xd3) ||
	       (op >= 0xd8 && op <= 0xdf) || op == 0xf6 || op == 0xf7 || op == 0xfe || op == 0xff;
}

[[nodiscard]] constexpr bool TwoByteHasModRm(uint8_t op) {
	return !(op == 0x05 || op == 0x06 || op == 0x07 || op == 0x08 || op == 0x09 || op == 0x0b ||
	         op == 0x0e || (op >= 0x30 && op <= 0x37) || op == 0x77 || (op >= 0x80 && op <= 0x8f) ||
	         op == 0xa0 || op == 0xa1 || op == 0xa2 || op == 0xa8 || op == 0xa9 || op == 0xaa ||
	         (op >= 0xc8 && op <= 0xcf));
}

} // namespace Detail

// The operand's address, from the instruction's first bytes (15 at most are read).
[[nodiscard]] inline bool MemoryOperandAddress(const std::array<uint8_t, 16>& code,
                                               const Registers& regs, uint64_t* address) {
	size_t i = 0;
	while (i < 4) {
		const auto c = code[i];
		if (c == 0x64 || c == 0x65 || c == 0x67) {
			return false; // FS or GS: an offset into a segment; 0x67: 32-bit addressing
		}
		if (c == 0x66 || c == 0xf0 || c == 0xf2 || c == 0xf3 || c == 0x2e || c == 0x3e ||
		    c == 0x26 || c == 0x36) {
			i++;
			continue;
		}
		break;
	}
	bool rex_x = false;
	bool rex_b = false;
	if ((code[i] & 0xf0u) == 0x40u) {
		rex_x = (code[i] & 0x02u) != 0;
		rex_b = (code[i] & 0x01u) != 0;
		i++;
	}
	bool modrm = false;
	if (code[i] == 0xc5 || code[i] == 0xc4) {
		const bool three = code[i] == 0xc4;
		const auto map   = three ? (code[i + 1] & 0x1fu) : 1u;
		const auto op    = code[i + (three ? 3 : 2)];
		if ((map == 1 && op == 0x77) || (map == 2 && op >= 0x90 && op <= 0x93)) {
			return false; // VZEROUPPER/VZEROALL (no ModRM); gathers (a vector index)
		}
		if (three) {
			rex_x = (code[i + 1] & 0x40u) == 0; // stored inverted
			rex_b = (code[i + 1] & 0x20u) == 0;
		}
		i += three ? 4 : 3; // the VEX prefix, its payload, the opcode
		modrm = true;
	} else if (code[i] == 0x0f) {
		const auto op = code[i + 1];
		if (op >= 0x19 && op <= 0x1f) {
			return false; // the multi-byte NOPs and hints: they read nothing
		}
		if (op == 0x38 || op == 0x3a) {
			i += 3;
			modrm = true;
		} else {
			i += 2;
			modrm = Detail::TwoByteHasModRm(op);
		}
	} else {
		const auto op = code[i];
		if (op == 0x8d) {
			return false; // LEA computes an address, it reads nothing
		}
		i++;
		modrm = Detail::OneByteHasModRm(op);
	}
	if (!modrm || i + 6 > code.size()) {
		return false;
	}
	const auto mod = static_cast<uint32_t>(code[i] >> 6u);
	const auto rm  = static_cast<uint32_t>(code[i] & 0x07u);
	i++;
	if (mod == 3) {
		return false; // a register
	}
	uint64_t ea          = 0;
	bool     disp32_only = false;
	if (rm == 4) {
		const auto sib   = code[i++];
		const auto index = ((sib >> 3u) & 0x07u) | (rex_x ? 0x08u : 0u);
		const auto base  = (sib & 0x07u) | (rex_b ? 0x08u : 0u);
		if (index != 4) {
			ea += regs[index] << (sib >> 6u);
		}
		if ((sib & 0x07u) == 5 && mod == 0) {
			disp32_only = true;
		} else {
			ea += regs[base];
		}
	} else if (rm == 5 && mod == 0) {
		return false; // RIP-relative: the instruction's length is not worked out here
	} else {
		ea += regs[rm | (rex_b ? 0x08u : 0u)];
	}
	if (mod == 1) {
		ea += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(code[i])));
	} else if (mod == 2 || disp32_only) {
		int32_t disp = 0;
		std::memcpy(&disp, &code[i], sizeof(disp));
		ea += static_cast<uint64_t>(static_cast<int64_t>(disp));
	}
	*address = ea;
	return true;
}

} // namespace Common::X64

#endif // KYTY_COMMON_X64MEMORYOPERAND_H_
