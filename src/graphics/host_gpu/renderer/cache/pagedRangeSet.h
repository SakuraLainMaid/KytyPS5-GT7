#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDRANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDRANGESET_H_

#include "graphics/host_gpu/rangeSet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics {

// A RangeSet with a bitmap of the 4 KB pages its ranges touch, so that the common question "does
// this small range hold any of its bytes" is answered without the tree's search when no page of
// the range is marked (round 80). The GPU-written byte ranges of the buffer cache are asked about
// for every guest read of the SRT walks and of the prepared draws' checks (HasGpuDirtyBytes): the
// tree's lower_bound was the GPU thread's busiest line in round 79's races (2 % of its time, its
// nodes cache misses). A marked page may hold none of the bytes any more (a page is unmarked once
// nothing is left on it, and only where a subtraction ends), so a marked page goes to the tree:
// "no" from the bitmap is exact, "yes" is checked.
//
// Not thread-safe: the owner guards it as it guards a RangeSet (the reads change nothing).
class PagedRangeSet final {
public:
	// KYTY_BISECT's dirtyindex:off asks the tree every time, as before round 80.
	static void SetIndexOff(bool off) noexcept {
		s_index_off.store(off, std::memory_order_relaxed);
	}

	void Add(uint64_t address, uint64_t size) {
		m_ranges.Add(address, size);
		if (size == 0 || size > UINT64_MAX - address) {
			return;
		}
		ForEachPageWord(PageOf(address), PageOf(address + size - 1), true,
		                [](uint64_t& word, uint64_t mask) { word |= mask; });
	}

	void Subtract(uint64_t address, uint64_t size) {
		m_ranges.Subtract(address, size);
		if (size == 0 || size > UINT64_MAX - address || m_chunks.empty()) {
			return;
		}
		const auto end   = address + size;
		auto       first = PageOf(address);
		auto       last  = PageOf(end - 1);
		// A page the subtraction covers only in part may keep bytes of a range outside it.
		const bool partial_first = (address & PageMask) != 0;
		const bool partial_last  = (end & PageMask) != 0;
		if (first == last) {
			if (partial_first || partial_last) {
				ClearIfEmpty(first);
			} else {
				ClearPages(first, first);
			}
			return;
		}
		if (partial_first) {
			ClearIfEmpty(first);
			first++;
		}
		if (partial_last) {
			ClearIfEmpty(last);
			last--;
		}
		if (first <= last) {
			ClearPages(first, last);
		}
	}

	void Clear() {
		m_ranges.Clear();
		m_chunks.clear();
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		if (size == 0 || size > UINT64_MAX - address ||
		    s_index_off.load(std::memory_order_relaxed)) {
			return m_ranges.Intersects(address, size);
		}
		if (m_chunks.empty()) {
			return false;
		}
		const auto first = PageOf(address);
		const auto last  = PageOf(address + size - 1);
		// A long range: the tree's one search costs less than the bitmap's words.
		if (last - first >= MaxScanPages) {
			return m_ranges.Intersects(address, size);
		}
		return AnyPageMarked(first, last) && m_ranges.Intersects(address, size);
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		return m_ranges.Contains(address, size);
	}
	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		m_ranges.ForEachInRange(address, size, std::forward<Func>(func));
	}
	template <typename Func>
	void ForEachOverlapping(uint64_t address, uint64_t size, Func&& func) const {
		m_ranges.ForEachOverlapping(address, size, std::forward<Func>(func));
	}
	template <typename Func>
	void ForEach(Func&& func) const {
		m_ranges.ForEach(std::forward<Func>(func));
	}
	[[nodiscard]] bool            Empty() const { return m_ranges.Empty(); }
	[[nodiscard]] const RangeSet& Ranges() const noexcept { return m_ranges; }

	// Diagnostics and tests: whether the page holding `address` is marked.
	[[nodiscard]] bool PageMarked(uint64_t address) const {
		return AnyPageMarked(PageOf(address), PageOf(address));
	}

private:
	static constexpr uint64_t PageBits      = 12;
	static constexpr uint64_t PageMask      = (uint64_t {1} << PageBits) - 1;
	static constexpr uint64_t ChunkPageBits = 12; // 4,096 pages (16 MB) a chunk
	static constexpr uint64_t ChunkPages    = uint64_t {1} << ChunkPageBits;
	static constexpr uint64_t ChunkWords    = ChunkPages / 64;
	static constexpr uint64_t MaxScanPages  = 2048; // 8 MB: at most 32 words and two chunks
	static constexpr uint64_t LastPage      = UINT64_MAX >> PageBits;

	using Chunk = std::array<uint64_t, ChunkWords>;

	struct ChunkHash {
		size_t operator()(uint64_t index) const noexcept {
			const uint64_t mixed = index * 0x9e3779b97f4a7c15ull;
			return static_cast<size_t>(mixed ^ (mixed >> 29u));
		}
	};

	static uint64_t PageOf(uint64_t address) noexcept { return address >> PageBits; }

	[[nodiscard]] const Chunk* FindChunk(uint64_t index) const {
		const auto it = m_chunks.find(index);
		return it != m_chunks.end() ? it->second.get() : nullptr;
	}
	[[nodiscard]] Chunk* FindChunk(uint64_t index) {
		const auto it = m_chunks.find(index);
		return it != m_chunks.end() ? it->second.get() : nullptr;
	}
	Chunk* GetChunk(uint64_t index) {
		auto& slot = m_chunks[index];
		if (slot == nullptr) {
			slot = std::make_unique<Chunk>();
			slot->fill(0);
		}
		return slot.get();
	}

	// func(word, mask) for the bits of pages [first, last], chunk by chunk (chunks made when
	// `create`, skipped when absent otherwise).
	template <typename Func>
	void ForEachPageWord(uint64_t first, uint64_t last, bool create, Func&& func) {
		for (uint64_t page = first; page <= last;) {
			const auto chunk_index = page >> ChunkPageBits;
			const auto chunk_last  = std::min(last, ((chunk_index + 1) << ChunkPageBits) - 1);
			if (auto* chunk = create ? GetChunk(chunk_index) : FindChunk(chunk_index);
			    chunk != nullptr) {
				for (uint64_t p = page; p <= chunk_last;) {
					const auto bit   = p & (ChunkPages - 1);
					const auto low   = bit % 64;
					const auto count = std::min<uint64_t>(64 - low, chunk_last - p + 1);
					const auto mask =
					    count == 64 ? ~uint64_t {0} : ((uint64_t {1} << count) - 1) << low;
					func((*chunk)[bit / 64], mask);
					p += count;
				}
			}
			if (chunk_last >= LastPage) {
				break;
			}
			page = chunk_last + 1;
		}
	}

	[[nodiscard]] bool AnyPageMarked(uint64_t first, uint64_t last) const {
		for (uint64_t page = first; page <= last;) {
			const auto chunk_index = page >> ChunkPageBits;
			const auto chunk_last  = std::min(last, ((chunk_index + 1) << ChunkPageBits) - 1);
			if (const auto* chunk = FindChunk(chunk_index); chunk != nullptr) {
				for (uint64_t p = page; p <= chunk_last;) {
					const auto bit   = p & (ChunkPages - 1);
					const auto low   = bit % 64;
					const auto count = std::min<uint64_t>(64 - low, chunk_last - p + 1);
					const auto mask =
					    count == 64 ? ~uint64_t {0} : ((uint64_t {1} << count) - 1) << low;
					if (((*chunk)[bit / 64] & mask) != 0) {
						return true;
					}
					p += count;
				}
			}
			if (chunk_last >= LastPage) {
				break;
			}
			page = chunk_last + 1;
		}
		return false;
	}

	void ClearPages(uint64_t first, uint64_t last) {
		ForEachPageWord(first, last, false, [](uint64_t& word, uint64_t mask) { word &= ~mask; });
	}

	void ClearIfEmpty(uint64_t page) {
		// The last page's 4 KB would wrap: up to its last byte but one, all a range can hold.
		const uint64_t size = page == LastPage ? PageMask : uint64_t {1} << PageBits;
		if (!m_ranges.Intersects(page << PageBits, size)) {
			ClearPages(page, page);
		}
	}

	RangeSet                                                        m_ranges;
	std::unordered_map<uint64_t, std::unique_ptr<Chunk>, ChunkHash> m_chunks;

	inline static std::atomic<bool> s_index_off {false};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDRANGESET_H_
