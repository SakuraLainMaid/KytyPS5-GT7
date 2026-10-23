#ifndef EMULATOR_INCLUDE_EMULATOR_COMMON_CRASHNOTE_H_
#define EMULATOR_INCLUDE_EMULATOR_COMMON_CRASHNOTE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace Common {

// Round 134: what a host thread is doing, for a crash report (a pipeline the graphics driver
// compiles, say): a fixed buffer, so the crash handler reads it without allocating. Empty when it
// is doing nothing of note.
inline thread_local std::array<char, 192> t_crash_note {};

inline void SetCrashNote(const char* text) noexcept {
	const auto length = std::min(std::strlen(text), t_crash_note.size() - 1);
	std::memcpy(t_crash_note.data(), text, length);
	t_crash_note[length] = '\0';
}

inline void ClearCrashNote() noexcept {
	t_crash_note[0] = '\0';
}

[[nodiscard]] inline const char* CrashNote() noexcept {
	return t_crash_note.data();
}

// Sets the note for a scope.
class CrashNoteScope {
public:
	explicit CrashNoteScope(const char* text) noexcept { SetCrashNote(text); }
	~CrashNoteScope() { ClearCrashNote(); }
	CrashNoteScope(const CrashNoteScope&)            = delete;
	CrashNoteScope& operator=(const CrashNoteScope&) = delete;
};

} // namespace Common

#endif // EMULATOR_INCLUDE_EMULATOR_COMMON_CRASHNOTE_H_
