#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGEZONESTAMPS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGEZONESTAMPS_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

// Where images were registered or unregistered, by zone of guest memory (round 78). Each change
// stamps the zones its image's memory covers with a new, larger value; the stamp of a range is the
// largest stamp of its zones. While a range's stamp stays the same, no image over that range was
// made or dropped, so a lookup of an image description there finds what it found before (the
// texture memo, TextureCache::RangeStamp). Before, one generation for all of guest memory moved
// with every image, and GT7 makes and drops hundreds of small images a frame in its per-frame
// memory (light grids): every texture binding kept for reuse was looked up again a few times a
// frame.
//
// Zones of 1 MB for a range of up to 4 zones, regions of 64 MB for a longer one (a region is
// stamped with each change in any of its zones). Both tables are hashed, direct-mapped: zones
// that share a slot only make a range's stamp move when it did not have to, never stay when it
// had to.
class ImageZoneStamps {
public:
	static constexpr uint64_t ZoneBits    = 20;
	static constexpr uint64_t RegionBits  = 26;
	static constexpr uint64_t SmallZones  = 4;
	static constexpr uint64_t MaxRegions  = 64;   // past it a range has no stamp that stays
	static constexpr uint64_t MaxStamped  = 4096; // zones a change stamps one by one, at most
	static constexpr uint64_t ZoneSlots   = uint64_t {1} << 16u;
	static constexpr uint64_t RegionSlots = uint64_t {1} << 12u;

	// An image over [address, address + size) was registered or unregistered.
	void Stamp(uint64_t address, uint64_t size) noexcept {
		if (size == 0) {
			return;
		}
		const auto stamp = m_counter.fetch_add(1, std::memory_order_acq_rel) + 1;
		const auto first = address >> ZoneBits;
		const auto last  = (address + size - 1) >> ZoneBits;
		if (last - first + 1 > MaxStamped) {
			// A huge image: every range's stamp moves.
			Raise(m_floor, stamp);
			return;
		}
		for (auto zone = first; zone <= last; zone++) {
			Raise(m_zones[ZoneSlot(zone)], stamp);
		}
		for (auto region = address >> RegionBits; region <= (address + size - 1) >> RegionBits;
		     region++) {
			Raise(m_regions[RegionSlot(region)], stamp);
		}
	}

	// The stamp of [address, address + size): the same again only while nothing was stamped
	// over the range.
	[[nodiscard]] uint64_t RangeStamp(uint64_t address, uint64_t size) const noexcept {
		uint64_t stamp = m_floor.load(std::memory_order_acquire);
		if (size == 0) {
			return stamp;
		}
		const auto first = address >> ZoneBits;
		const auto last  = (address + size - 1) >> ZoneBits;
		if (last - first + 1 <= SmallZones) {
			for (auto zone = first; zone <= last; zone++) {
				stamp = std::max(stamp, m_zones[ZoneSlot(zone)].load(std::memory_order_acquire));
			}
			return stamp;
		}
		const auto first_region = address >> RegionBits;
		const auto last_region  = (address + size - 1) >> RegionBits;
		if (last_region - first_region + 1 > MaxRegions) {
			// Too long to look at: a new stamp each time (never the same twice).
			return m_counter.fetch_add(1, std::memory_order_acq_rel) + 1;
		}
		for (auto region = first_region; region <= last_region; region++) {
			stamp = std::max(stamp, m_regions[RegionSlot(region)].load(std::memory_order_acquire));
		}
		return stamp;
	}

private:
	// A slot's stamp only ever grows, whatever order concurrent changes store theirs in: a slot
	// two zones share then holds the newer of their stamps.
	static void Raise(std::atomic<uint64_t>& slot, uint64_t stamp) noexcept {
		auto current = slot.load(std::memory_order_relaxed);
		while (current < stamp &&
		       !slot.compare_exchange_weak(current, stamp, std::memory_order_acq_rel,
		                                   std::memory_order_relaxed)) {
		}
	}
	// Fibonacci hashing of a zone or region number into a table slot.
	static constexpr size_t Slot(uint64_t number, uint32_t slot_bits) noexcept {
		return static_cast<size_t>((number * 0x9E3779B97F4A7C15ull) >> (64u - slot_bits));
	}
	static constexpr size_t ZoneSlot(uint64_t zone) noexcept { return Slot(zone, 16u); }
	static constexpr size_t RegionSlot(uint64_t region) noexcept { return Slot(region, 12u); }

	std::array<std::atomic<uint64_t>, ZoneSlots>   m_zones {};
	std::array<std::atomic<uint64_t>, RegionSlots> m_regions {};
	mutable std::atomic<uint64_t>                  m_counter {1};
	std::atomic<uint64_t>                          m_floor {1};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGEZONESTAMPS_H_
