#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWREPEATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWREPEATS_H_

#include "common/logging/log.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// KYTY_DRAW_REPEATS=1 (round 83; dispatches, older frames and the finer counts round 113): how
// much of each frame's work earlier frames did too, the share a memo of a draw's or a dispatch's
// prepared state could serve. Round 83 found no draw whose inputs the frame just before had: the
// user data points at memory that changes each frame, which a game often cycles through two or
// three copies of, so the frames before that are counted too. Each draw or dispatch gives three
// keys: its inputs (the stages' shaders and user data), what was resolved from them (the
// descriptors the shaders read, the flattened tables, the targets, the index and vertex buffers or
// the thread groups), and its textures alone (the shaders, image and sampler descriptors and the
// targets, no buffers). Every 10 s a "Draw repeats" and a "Dispatch repeats" line (GPU thread)
// give, for 1 to 4 frames before: the same inputs, the same resolved state, the same resolved
// state at the same place in the frame (a memo by position); then inputs that resolve as they did
// the last time they were seen (a memo by inputs), the same textures as 1 frame before, the same
// resolved state as the one just before in the frame or any earlier one, and how often the
// texture cache's image set changed a frame (what a memo's images would be checked against).
namespace Libs::Graphics::DrawRepeats {

[[nodiscard]] inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DRAW_REPEATS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

class Stats {
public:
	static constexpr size_t Depth = 4; // the frames before compared with

	explicit Stats(const char* name): m_name(name) {}

	// GPU thread, once a draw or dispatch is prepared.
	void Note(uint64_t frame, uint64_t inputs, uint64_t resolved, uint64_t textures,
	          uint64_t image_generation) {
		if (frame != m_frame) {
			if (m_frame != UINT64_MAX && image_generation >= m_frame_generation) {
				m_image_changes += image_generation - m_frame_generation;
			}
			m_frame            = frame;
			m_frame_generation = image_generation;
			m_frames++;
			// [0] is this frame, [d] the one d frames before.
			for (size_t d = Depth; d > 0; d--) {
				std::swap(m_history[d], m_history[d - 1]);
			}
			m_history[0].inputs.clear();
			m_history[0].resolved.clear();
			m_history[0].textures.clear();
			m_history[0].position.clear();
			m_last_resolved = 0;
		}
		m_count++;
		auto&      now   = m_history[0];
		const auto place = now.position.size();
		for (size_t d = 1; d <= Depth; d++) {
			const auto& then = m_history[d];
			m_input_hits[d - 1] += then.inputs.contains(inputs) ? 1u : 0u;
			m_resolved_hits[d - 1] += then.resolved.contains(resolved) ? 1u : 0u;
			m_position_hits[d - 1] +=
			    place < then.position.size() && then.position[place] == resolved ? 1u : 0u;
		}
		// The last time these inputs were seen, in this frame or the ones before.
		for (size_t d = 0; d <= Depth; d++) {
			const auto found = m_history[d].inputs.find(inputs);
			if (found != m_history[d].inputs.end()) {
				m_input_seen++;
				m_input_stable += found->second == resolved ? 1u : 0u;
				break;
			}
		}
		m_texture_hits += m_history[1].textures.contains(textures) ? 1u : 0u;
		m_consecutive += m_last_resolved == resolved ? 1u : 0u;
		m_same_frame += now.resolved.insert(resolved).second ? 0u : 1u;
		now.inputs[inputs] = resolved;
		now.textures.insert(textures);
		now.position.push_back(resolved);
		m_last_resolved = resolved;
		ReportIfDue();
	}

private:
	struct Frame {
		std::unordered_map<uint64_t, uint64_t> inputs; // inputs -> what they resolved to
		std::unordered_set<uint64_t>           resolved;
		std::unordered_set<uint64_t>           textures;
		std::vector<uint64_t>                  position; // resolved, by place in the frame
	};

	void ReportIfDue() {
		const auto now = std::chrono::steady_clock::now();
		if (now - m_last < std::chrono::seconds(10)) {
			return;
		}
		const auto percent = [&](uint64_t n) {
			return m_count != 0 ? 100.0 * static_cast<double>(n) / static_cast<double>(m_count)
			                    : 0.0;
		};
		const auto by_distance = [&](const std::array<uint64_t, Depth>& hits) {
			std::string text;
			for (size_t d = 0; d < Depth; d++) {
				char part[32];
				std::snprintf(part, sizeof(part), "%s%.1f%%", d == 0 ? "" : " ", percent(hits[d]));
				text += part;
			}
			return text;
		};
		const auto frames = m_frames != 0 ? m_frames : 1u;
		LOGF("%s repeats (%.0f s): %" PRIu64 " in %" PRIu64 " frames (%.0f a frame); 1, 2, 3 and 4 "
		     "frames before had the same inputs (shaders, user data) for %s, the same resolved "
		     "descriptors, targets and buffers for %s, the same at the same place in the frame "
		     "for %s; inputs seen before %.1f%% (resolved the same %.1f%%); the same textures and "
		     "targets as the frame before %.1f%%; %.1f%% repeat the one just before, %.1f%% one "
		     "earlier in their frame; image set changes a frame %.1f\n",
		     m_name, std::chrono::duration<double>(now - m_last).count(), m_count, m_frames,
		     static_cast<double>(m_count) / static_cast<double>(frames),
		     by_distance(m_input_hits).c_str(), by_distance(m_resolved_hits).c_str(),
		     by_distance(m_position_hits).c_str(), percent(m_input_seen), percent(m_input_stable),
		     percent(m_texture_hits), percent(m_consecutive), percent(m_same_frame),
		     static_cast<double>(m_image_changes) / static_cast<double>(frames));
		m_last          = now;
		m_count         = 0;
		m_frames        = 0;
		m_input_hits    = {};
		m_resolved_hits = {};
		m_position_hits = {};
		m_input_seen    = 0;
		m_input_stable  = 0;
		m_texture_hits  = 0;
		m_consecutive   = 0;
		m_same_frame    = 0;
		m_image_changes = 0;
	}

	const char*                           m_name;
	std::array<Frame, Depth + 1>          m_history;
	uint64_t                              m_frame            = UINT64_MAX;
	uint64_t                              m_frame_generation = 0;
	uint64_t                              m_last_resolved    = 0;
	uint64_t                              m_count            = 0;
	uint64_t                              m_frames           = 0;
	std::array<uint64_t, Depth>           m_input_hits {};
	std::array<uint64_t, Depth>           m_resolved_hits {};
	std::array<uint64_t, Depth>           m_position_hits {};
	uint64_t                              m_input_seen    = 0;
	uint64_t                              m_input_stable  = 0;
	uint64_t                              m_texture_hits  = 0;
	uint64_t                              m_consecutive   = 0;
	uint64_t                              m_same_frame    = 0;
	uint64_t                              m_image_changes = 0;
	std::chrono::steady_clock::time_point m_last          = std::chrono::steady_clock::now();
};

} // namespace Libs::Graphics::DrawRepeats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWREPEATS_H_
