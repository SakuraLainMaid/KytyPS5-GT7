#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITETICKS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITETICKS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

namespace Libs::Graphics {

// Ranges of guest memory and the scheduler tick of the last GPU write recorded to each
// (BufferCache::MarkGpuWritten): a later write takes its range over, whatever wrote it before.
// What a GPU-written range holds is final once the GPU has finished its tick (the fast readback,
// BufferCache::FastReadback). Past MaxEntries the map starts again from a floor: every range it
// forgot counts as written in the latest tick it had seen.
class WriteTicks final {
public:
	static constexpr size_t MaxEntries = 65536;

	void Assign(uint64_t begin, uint64_t end, uint64_t tick) {
		if (begin >= end) {
			return;
		}
		m_latest = std::max(m_latest, tick);
		if (m_ranges.size() >= MaxEntries) {
			m_floor = m_latest;
			m_ranges.clear();
		}
		// An entry that starts before the range and reaches into it keeps its part before (and
		// after, when it reaches past the range).
		auto it = m_ranges.lower_bound(begin);
		if (it != m_ranges.begin()) {
			const auto previous = std::prev(it);
			if (previous->second.end > begin) {
				const auto old       = previous->second;
				previous->second.end = begin;
				if (old.end > end) {
					m_ranges.emplace(end, Entry {old.end, old.tick});
				}
			}
		}
		// The entries that start inside it go; one that reaches past it keeps that part.
		it = m_ranges.lower_bound(begin);
		while (it != m_ranges.end() && it->first < end) {
			if (it->second.end > end) {
				const auto old = it->second;
				m_ranges.erase(it);
				m_ranges.emplace(end, old);
				break;
			}
			it = m_ranges.erase(it);
		}
		// Neighbours of the same tick (one command buffer's writes) become one entry.
		auto start = begin;
		auto stop  = end;
		it         = m_ranges.lower_bound(begin);
		if (it != m_ranges.begin()) {
			const auto previous = std::prev(it);
			if (previous->second.end == begin && previous->second.tick == tick) {
				start = previous->first;
				m_ranges.erase(previous);
			}
		}
		if (const auto next = m_ranges.find(end);
		    next != m_ranges.end() && next->second.tick == tick) {
			stop = next->second.end;
			m_ranges.erase(next);
		}
		m_ranges.emplace(start, Entry {stop, tick});
	}

	// The latest tick among the writes over [begin, end), at least the floor.
	[[nodiscard]] uint64_t Max(uint64_t begin, uint64_t end) const {
		uint64_t result = m_floor;
		if (begin >= end) {
			return result;
		}
		auto it = m_ranges.upper_bound(begin);
		if (it != m_ranges.begin()) {
			const auto previous = std::prev(it);
			if (previous->second.end > begin) {
				result = std::max(result, previous->second.tick);
			}
		}
		for (; it != m_ranges.end() && it->first < end; ++it) {
			result = std::max(result, it->second.tick);
		}
		return result;
	}

	[[nodiscard]] size_t Size() const noexcept { return m_ranges.size(); }

private:
	struct Entry {
		uint64_t end  = 0;
		uint64_t tick = 0;
	};
	std::map<uint64_t, Entry> m_ranges;
	uint64_t                  m_floor  = 0;
	uint64_t                  m_latest = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITETICKS_H_
