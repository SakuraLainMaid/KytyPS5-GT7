#include "graphics/host_gpu/renderer/stallWatch.h"

#include "common/logging/log.h"
#include "common/perfStats.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <fmt/format.h>

namespace Libs::Graphics::StallWatch {

namespace {

// Reports: the first 8 at least 2 s apart, then 5 s apart up to FullReports, then 30 s apart.
constexpr uint64_t FullReports  = 40;
constexpr uint64_t Millisecond  = 1'000'000;
constexpr size_t   SectionRing  = 512;
constexpr uint64_t MaxOwnRows   = 48; // this timeline's command buffers in one report
constexpr uint64_t MaxOtherRows = 12; // another timeline's

uint64_t ThresholdNs() {
	static const uint64_t ns = [] {
		const char* value = std::getenv("KYTY_STALL_MS");
		const auto  ms    = value != nullptr ? std::strtoull(value, nullptr, 10) : uint64_t {100};
		return std::max<uint64_t>(ms, 10) * Millisecond;
	}();
	return ns;
}

struct SectionRecord {
	uint64_t begin_ns = 0;
	uint64_t end_ns   = 0;
	uint64_t arg      = 0;
	uint32_t tid      = 0;
	Section  section  = Section::Count;
};

std::mutex                             g_sections_mutex;
std::array<SectionRecord, SectionRing> g_sections {};
uint64_t                               g_section_count = 0;

std::mutex             g_registry_mutex;
std::vector<Timeline*> g_registry;

std::mutex g_report_mutex;
uint64_t   g_reports        = 0;
uint64_t   g_last_report_ns = 0;

const char* SectionName(Section section) {
	switch (section) {
		case Section::Acquire: return "swapchain acquire";
		case Section::Present: return "present (queue lock and vkQueuePresentKHR)";
		case Section::QueueLock: return "waiting for the queue lock to submit";
		case Section::QueueSubmit: return "vkQueueSubmit";
		case Section::Record: return "recording thread chunk";
		case Section::Callback: return "priority callback";
		case Section::OwnerWait: return "scheduler thread waiting for the GPU";
		case Section::PriorityWait: return "GPU thread waiting for priority operations";
		case Section::BufferGc: return "buffer cache collector";
		case Section::TextureGc: return "texture cache collector";
		case Section::Count: break;
	}
	return "?";
}

const char* TransferName(size_t kind) {
	switch (static_cast<Transfer>(kind)) {
		case Transfer::BufferUpload: return "buffer upload";
		case Transfer::BufferDownload: return "buffer download";
		case Transfer::BufferCopy: return "buffer copy";
		case Transfer::ImageUpload: return "image upload";
		case Transfer::ImageDownload: return "image download";
		case Transfer::ImageCopy: return "image copy";
		case Transfer::Detile: return "detile";
		case Transfer::Tile: return "tile";
		case Transfer::NanScan: return "NaN scan";
		case Transfer::Other: return "other";
		case Transfer::Count: break;
	}
	return "?";
}

const char* KindName(char kind) {
	switch (kind) {
		case 'd': return "draw";
		case 'D': return "indirect draw";
		case 'c': return "dispatch";
		case 'C': return "indirect dispatch";
		default: return "?";
	}
}

double Ms(int64_t ns) {
	return static_cast<double>(ns) / 1e6;
}

int64_t Signed(uint64_t value, uint64_t reference) {
	return value >= reference ? static_cast<int64_t>(value - reference)
	                          : -static_cast<int64_t>(reference - value);
}

std::string Bytes(uint64_t bytes) {
	if (bytes >= (uint64_t {1} << 20u)) {
		return fmt::format("{:.1f} MB", static_cast<double>(bytes) / 1048576.0);
	}
	return fmt::format("{} KB", (bytes + 1023u) >> 10u);
}

std::mutex  g_memory_mutex;
std::string g_memory_text;

std::atomic<Timeline::GpuTimes> g_test_times {nullptr};

std::string MemoryText() {
	std::scoped_lock lock(g_memory_mutex);
	return g_memory_text;
}

} // namespace

void UpdateMemoryText(const GraphicContext& graphics) {
	static std::atomic<uint64_t> next_ns {0};
	const auto                   now  = NowNs();
	auto                         next = next_ns.load(std::memory_order_relaxed);
	if (now < next || !next_ns.compare_exchange_strong(next, now + 500 * Millisecond,
	                                                   std::memory_order_relaxed)) {
		return;
	}
	if (!graphics.CanReportMemoryUsage()) {
		return;
	}
	const auto gb   = [](uint64_t bytes) { return static_cast<double>(bytes) / 1073741824.0; };
	auto       text = fmt::format(
        " | VRAM {:.1f} GB used of a {:.1f} GB budget (the emulator plans for {:.1f} GB, short "
	          "{:.1f} GB)",
        gb(graphics.GetDeviceMemoryUsage()), gb(graphics.GetRawMemoryBudget()),
        gb(graphics.GetTotalMemoryBudget()), gb(graphics.MemoryShortfall()));
	{
		std::scoped_lock lock(g_memory_mutex);
		g_memory_text = std::move(text);
	}
	static std::once_flag registered;
	std::call_once(registered, [] { Common::PerfStats::SetExtraReporter(&MemoryText); });
}

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_STALL_WATCH");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

namespace Detail {

void NoteSectionSlow(Section section, uint64_t begin_ns, uint64_t end_ns, uint64_t arg) {
	if (!Enabled()) {
		return;
	}
	const auto       tid = Common::PerfStats::CurrentOsThreadId();
	std::scoped_lock lock(g_sections_mutex);
	g_sections[g_section_count++ % SectionRing] = {begin_ns, end_ns, arg, tid, section};
}

} // namespace Detail

Timeline::Timeline(GraphicContext& graphics, std::string name)
    : m_graphics(graphics), m_name(std::move(name)), m_records(Slots) {
	const auto& limits = graphics.physical_device_properties.limits;
	if (limits.timestampComputeAndGraphics && limits.timestampPeriod > 0.0f) {
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = static_cast<uint32_t>(Slots * 2);
		if (graphics.device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess) {
			m_pool = nullptr;
		} else {
			m_period_ns = static_cast<double>(limits.timestampPeriod);
		}
	}
	std::scoped_lock lock(g_registry_mutex);
	g_registry.push_back(this);
}

Timeline::~Timeline() {
	{
		std::scoped_lock lock(g_registry_mutex);
		g_registry.erase(std::remove(g_registry.begin(), g_registry.end(), this), g_registry.end());
	}
	if (m_pool) {
		m_graphics.device.destroyQueryPool(m_pool, nullptr);
	}
}

void Timeline::SetGpuTimesForTests(GpuTimes times) {
	g_test_times.store(times, std::memory_order_release);
}

void Timeline::SetName(std::string name) {
	m_name = std::move(name);
}

void Timeline::Open(uint64_t tick) {
	m_open      = {};
	m_open_tick = tick;
	m_open_ns   = NowNs();
}

void Timeline::AddShader(uint64_t shader, char kind) {
	auto& content = m_open;
	for (size_t i = 0; i < MaxShaders; i++) {
		if (content.counts[i] == 0) {
			content.shaders[i] = shader;
			content.kinds[i]   = kind;
			content.counts[i]  = 1;
			return;
		}
		if (content.shaders[i] == shader && content.kinds[i] == kind) {
			content.counts[i]++;
			return;
		}
	}
	content.other_shaders++;
}

void Timeline::NoteDraw(uint64_t shader, bool indirect) {
	m_open.draws++;
	m_open.indirect += indirect ? 1u : 0u;
	AddShader(shader, indirect ? 'D' : 'd');
}

void Timeline::NoteDispatch(uint64_t shader, bool indirect, uint64_t groups) {
	m_open.dispatches++;
	m_open.indirect += indirect ? 1u : 0u;
	if (groups > m_open.largest_groups) {
		m_open.largest_groups = groups;
		m_open.largest_shader = shader;
	}
	AddShader(shader, indirect ? 'C' : 'c');
}

void Timeline::NoteTransfer(Transfer kind, uint64_t bytes) {
	const auto index = std::min(static_cast<size_t>(kind), static_cast<size_t>(Transfer::Other));
	m_open.transfers[index]++;
	m_open.bytes[index] += bytes;
}

void Timeline::Close(uint64_t tick) {
	const auto       now = NowNs();
	std::scoped_lock lock(m_mutex);
	auto&            record = m_records[tick % Slots];
	record.tick             = tick;
	record.opened_ns        = m_open_tick == tick ? m_open_ns : now;
	record.closed_ns        = now;
	record.queued_ns        = 0;
	record.content          = m_open;
	m_last_closed           = std::max(m_last_closed, tick);
}

void Timeline::WriteBegin(vk::CommandBuffer command, uint64_t tick) const {
	if (!m_pool) {
		return;
	}
	const auto slot = static_cast<uint32_t>(tick % Slots);
	command.resetQueryPool(m_pool, slot * 2u, 2);
	command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_pool, slot * 2u);
}

void Timeline::WriteEnd(vk::CommandBuffer command, uint64_t tick) const {
	if (!m_pool) {
		return;
	}
	const auto slot = static_cast<uint32_t>(tick % Slots);
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, slot * 2u + 1u);
}

void Timeline::Queued(uint64_t tick) {
	const auto       now = NowNs();
	std::scoped_lock lock(m_mutex);
	auto&            record = m_records[tick % Slots];
	if (record.tick == tick) {
		record.queued_ns = now;
	}
}

void Timeline::Collect(uint64_t first, uint64_t last, std::vector<Row>& rows) const {
	std::vector<Record> records;
	{
		std::scoped_lock lock(m_mutex);
		for (auto tick = first; tick <= last && tick != 0; tick++) {
			const auto& record = m_records[tick % Slots];
			if (record.tick == tick) {
				records.push_back(record);
			}
		}
	}
	const auto test_times = g_test_times.load(std::memory_order_acquire);
	for (const auto& record: records) {
		Row row;
		row.timeline = this;
		row.record   = record;
		if (test_times != nullptr) {
			row.has_gpu = test_times(*this, record.tick, row.gpu_begin, row.gpu_end);
		} else if (m_pool) {
			// begin, its availability, end, its availability.
			std::array<uint64_t, 4> values {};
			const auto              slot   = static_cast<uint32_t>(record.tick % Slots);
			const auto              result = m_graphics.device.getQueryPoolResults(
                m_pool, slot * 2u, 2, sizeof(values), values.data(), sizeof(uint64_t) * 2,
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
			if ((result == vk::Result::eSuccess || result == vk::Result::eNotReady) &&
			    values[1] != 0 && values[3] != 0 && values[2] >= values[0]) {
				row.has_gpu   = true;
				row.gpu_begin = static_cast<uint64_t>(static_cast<double>(values[0]) * m_period_ns);
				row.gpu_end   = static_cast<uint64_t>(static_cast<double>(values[2]) * m_period_ns);
			}
		}
		rows.push_back(row);
	}
}

std::string Timeline::Describe(const Content& content) const {
	const auto plural = [](uint64_t count, const char* one, const char* many) {
		return fmt::format("{} {}", count, count == 1 ? one : many);
	};
	std::string text = plural(content.draws, "draw", "draws") + ", " +
	                   plural(content.dispatches, "dispatch", "dispatches");
	if (content.indirect != 0) {
		text += fmt::format(" ({} of them indirect)", content.indirect);
	}
	for (size_t kind = 0; kind < content.transfers.size(); kind++) {
		if (content.transfers[kind] != 0) {
			text += fmt::format(", {} x{}", TransferName(kind), content.transfers[kind]);
			if (content.bytes[kind] != 0) {
				text += fmt::format(" ({})", Bytes(content.bytes[kind]));
			}
		}
	}
	if (content.largest_groups > 1) {
		text += fmt::format(", largest dispatch {} groups ({:016x})", content.largest_groups,
		                    content.largest_shader);
	}
	for (size_t i = 0; i < MaxShaders && content.counts[i] != 0; i++) {
		text += fmt::format("{} {:016x} {} x{}", i == 0 ? " |" : ",", content.shaders[i],
		                    KindName(content.kinds[i]), content.counts[i]);
	}
	if (content.other_shaders != 0) {
		text += fmt::format(", {} more", content.other_shaders);
	}
	return text;
}

void Timeline::OnWait(uint64_t tick, uint64_t known, uint64_t waited_ns) {
	if (!Enabled() || waited_ns < ThresholdNs()) {
		return;
	}
	{
		// One stall, however many threads waited through it: counted when it began after the
		// last one counted had ended.
		static std::atomic<uint64_t> last_end {0};
		const auto                   end      = NowNs();
		const auto                   begin    = end > waited_ns ? end - waited_ns : 0;
		auto                         previous = last_end.load(std::memory_order_relaxed);
		while (previous < end &&
		       !last_end.compare_exchange_weak(previous, end, std::memory_order_relaxed)) {
		}
		Common::PerfStats::AddStall(waited_ns, begin >= previous);
	}
	uint64_t number = 0;
	{
		std::scoped_lock lock(g_report_mutex);
		const auto       now = NowNs();
		// Several threads wait for the same stall: one report for it.
		const uint64_t spacing = g_reports < 8             ? 2'000 * Millisecond
		                         : g_reports < FullReports ? 5'000 * Millisecond
		                                                   : 30'000 * Millisecond;
		if (g_last_report_ns != 0 && now - g_last_report_ns < spacing) {
			return;
		}
		g_last_report_ns = now;
		number           = ++g_reports;
	}
	Report(number, tick, known, waited_ns);
}

void Timeline::Report(uint64_t number, uint64_t tick, uint64_t known, uint64_t waited_ns) const {
	const auto now    = NowNs();
	const auto waiter = Common::PerfStats::ThreadNameOf(Common::PerfStats::CurrentOsThreadId());
	std::vector<Row> own;
	// A stall can leave hundreds of command buffers waiting behind the one that holds it.
	Collect(tick > 256 ? tick - 256 : 1, tick + 3, own);

	// The GPU's clock: the end of the waited-for command buffer, else the latest end seen.
	uint64_t reference = 0;
	bool     exact     = false;
	for (const auto& row: own) {
		if (!row.has_gpu) {
			continue;
		}
		if (row.record.tick == tick) {
			reference = row.gpu_end;
			exact     = true;
			break;
		}
		reference = std::max(reference, row.gpu_end);
	}
	const bool     timed       = reference != 0;
	const uint64_t window_from = timed && reference > waited_ns + 20 * Millisecond
	                                 ? reference - waited_ns - 20 * Millisecond
	                                 : 0;

	// This timeline's command buffers that matter: from a few before the one the GPU was on when
	// the wait began (or, without it, before those that ended in the wait) to the waited-for one
	// and the few after it.
	size_t from_index = own.size() > MaxOwnRows ? own.size() - MaxOwnRows : 0;
	{
		bool found = false;
		if (known != 0) {
			for (size_t i = 0; i < own.size(); i++) {
				if (own[i].record.tick + 3 >= known) {
					from_index = i;
					found      = true;
					break;
				}
			}
		}
		if (!found && timed) {
			for (size_t i = 0; i < own.size(); i++) {
				if (own[i].has_gpu && own[i].gpu_end >= window_from) {
					from_index = i >= 3 ? i - 3 : 0;
					break;
				}
			}
		}
	}
	std::vector<Row> rows(own.begin() + static_cast<std::ptrdiff_t>(from_index), own.end());

	// The other timelines' command buffers the GPU ran in that time.
	if (timed) {
		uint64_t window_begin = reference;
		for (const auto& row: rows) {
			if (row.has_gpu) {
				window_begin = std::min(window_begin, row.gpu_begin);
			}
		}
		std::vector<Timeline*> others;
		{
			std::scoped_lock lock(g_registry_mutex);
			for (auto* timeline: g_registry) {
				if (timeline != this) {
					others.push_back(timeline);
				}
			}
		}
		for (const auto* other: others) {
			uint64_t last = 0;
			{
				std::scoped_lock lock(other->m_mutex);
				last = other->m_last_closed;
			}
			if (last == 0) {
				continue;
			}
			std::vector<Row> candidates;
			other->Collect(last > 64 ? last - 64 : 1, last, candidates);
			std::vector<Row> overlapping;
			for (const auto& row: candidates) {
				if (row.has_gpu && row.gpu_end >= window_begin &&
				    row.gpu_begin <= reference + 2 * Millisecond) {
					overlapping.push_back(row);
				}
			}
			if (overlapping.size() > MaxOtherRows) {
				overlapping.erase(overlapping.begin(),
				                  overlapping.end() - static_cast<std::ptrdiff_t>(MaxOtherRows));
			}
			rows.insert(rows.end(), overlapping.begin(), overlapping.end());
		}
	}
	std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
		if (a.has_gpu != b.has_gpu) {
			return a.has_gpu;
		}
		return a.has_gpu ? a.gpu_begin < b.gpu_begin : a.record.tick < b.record.tick;
	});

	// The GPU's idle time before each one (from the latest end before it), the longest command
	// buffer and the longest idle time, and how much of the wait the GPU spent running something.
	std::vector<int64_t> idle(rows.size(), 0);
	size_t               longest = rows.size();
	size_t               idlest  = rows.size();
	{
		uint64_t previous_end = 0;
		for (size_t i = 0; i < rows.size(); i++) {
			const auto& row = rows[i];
			if (!row.has_gpu) {
				continue;
			}
			idle[i] = previous_end != 0 ? Signed(row.gpu_begin, previous_end) : 0;
			if (longest == rows.size() ||
			    row.gpu_end - row.gpu_begin > rows[longest].gpu_end - rows[longest].gpu_begin) {
				longest = i;
			}
			if (previous_end != 0 && (idlest == rows.size() || idle[i] > idle[idlest])) {
				idlest = i;
			}
			previous_end = std::max(previous_end, row.gpu_end);
		}
	}
	uint64_t busy_ns = 0;
	if (timed) {
		// The union of the command buffers' spans within the wait.
		const auto wait_begin = reference > waited_ns ? reference - waited_ns : 0;
		uint64_t   covered    = wait_begin;
		for (const auto& row: rows) {
			if (!row.has_gpu || row.gpu_end <= covered) {
				continue;
			}
			const auto from = std::max(row.gpu_begin, covered);
			const auto to   = std::min(row.gpu_end, reference);
			if (to > from) {
				busy_ns += to - from;
			}
			covered = std::max(covered, row.gpu_end);
		}
	}

	// Which to print: all of them when they are few, else the first ones, those around the
	// longest command buffer and the longest idle time, and the waited-for one.
	std::vector<bool> keep(rows.size(), rows.size() <= MaxOwnRows + MaxOtherRows);
	if (!keep.empty() && !keep[0]) {
		const auto mark = [&](size_t center, size_t before, size_t after) {
			if (center >= rows.size()) {
				return;
			}
			for (size_t i = center >= before ? center - before : 0;
			     i <= center + after && i < rows.size(); i++) {
				keep[i] = true;
			}
		};
		mark(0, 0, 5);
		mark(longest, 6, 6);
		mark(idlest, 6, 6);
		for (size_t i = 0; i < rows.size(); i++) {
			if (rows[i].timeline == this && rows[i].record.tick == tick) {
				mark(i, 2, 3);
			}
		}
		mark(rows.size() - 1, 2, 0);
	}

	std::string text = fmt::format(
	    "Stall {} ({}): {} waited {:.1f} ms for tick {}{}. {}, CPU times in ms before this "
	    "report:\n",
	    number, m_name, waiter, Ms(static_cast<int64_t>(waited_ns)), tick,
	    known != 0 ? fmt::format(" (the GPU had finished {} or later when the wait began)", known)
	               : std::string(),
	    !timed  ? std::string("No GPU times")
	    : exact ? fmt::format("GPU times in ms from when tick {} finished (length, idle before it)",
	                          tick)
	            : std::string("GPU times in ms from the latest end seen (length, idle before it)"));
	const auto cpu = [now](uint64_t ns) {
		return ns == 0 ? std::string("?") : fmt::format("{:+.1f}", Ms(Signed(ns, now)));
	};
	size_t left_out = 0;
	for (size_t i = 0; i < rows.size(); i++) {
		if (!keep[i]) {
			left_out++;
			continue;
		}
		if (left_out != 0) {
			text += fmt::format("Stall {}   ({} command buffers left out)\n", number, left_out);
			left_out = 0;
		}
		const auto& row    = rows[i];
		const auto& record = row.record;
		std::string gpu    = "gpu ?";
		if (row.has_gpu) {
			gpu = fmt::format("gpu {:+.2f}..{:+.2f} ({:.2f}, idle {:.2f})",
			                  Ms(Signed(row.gpu_begin, reference)),
			                  Ms(Signed(row.gpu_end, reference)),
			                  Ms(Signed(row.gpu_end, row.gpu_begin)), Ms(idle[i]));
		}
		text +=
		    fmt::format("Stall {}   {} {}{}: {} | cpu opened {}, closed {}, submitted {} | {}\n",
		                number, row.timeline->m_name, record.tick,
		                row.timeline == this && record.tick == tick ? " (waited for)" : "", gpu,
		                cpu(record.opened_ns), cpu(record.closed_ns), cpu(record.queued_ns),
		                row.timeline->Describe(record.content));
	}
	if (left_out != 0) {
		text += fmt::format("Stall {}   ({} command buffers left out)\n", number, left_out);
	}
	if (timed && longest < rows.size()) {
		const auto& row = rows[longest];
		text += fmt::format(
		    "Stall {} GPU: busy {:.1f} ms of the {:.1f} ms; the longest command buffer {} {} "
		    "({:.2f} ms: {})",
		    number, Ms(static_cast<int64_t>(busy_ns)), Ms(static_cast<int64_t>(waited_ns)),
		    row.timeline->m_name, row.record.tick, Ms(Signed(row.gpu_end, row.gpu_begin)),
		    row.timeline->Describe(row.record.content));
		if (idlest < rows.size()) {
			text += fmt::format("; the longest idle time {:.2f} ms, before {} {}", Ms(idle[idlest]),
			                    rows[idlest].timeline->m_name, rows[idlest].record.tick);
		}
		text += "\n";
	}

	// The slow CPU sections of that time.
	std::vector<SectionRecord> sections;
	{
		const auto from =
		    now > waited_ns + 50 * Millisecond ? now - waited_ns - 50 * Millisecond : 0;
		std::scoped_lock lock(g_sections_mutex);
		const auto       count = std::min<uint64_t>(g_section_count, SectionRing);
		for (uint64_t i = 0; i < count; i++) {
			const auto& section = g_sections[(g_section_count - count + i) % SectionRing];
			if (section.end_ns >= from) {
				sections.push_back(section);
			}
		}
	}
	std::ranges::sort(sections, {}, &SectionRecord::begin_ns);
	if (sections.size() > 32) {
		text += fmt::format("Stall {} CPU: {} slow sections, the 32 longest:\n", number,
		                    sections.size());
		std::ranges::sort(sections, std::greater {}, [](const SectionRecord& section) {
			return section.end_ns - section.begin_ns;
		});
		sections.resize(32);
		std::ranges::sort(sections, {}, &SectionRecord::begin_ns);
	}
	for (const auto& section: sections) {
		text +=
		    fmt::format("Stall {} CPU: {:+.1f} for {:.1f} ms: {} ({}{})\n", number,
		                Ms(Signed(section.begin_ns, now)),
		                Ms(static_cast<int64_t>(section.end_ns - section.begin_ns)),
		                SectionName(section.section), Common::PerfStats::ThreadNameOf(section.tid),
		                section.arg != 0 ? fmt::format(", {}", section.arg) : std::string());
	}
	if (sections.empty()) {
		text += fmt::format("Stall {} CPU: no section of 2 ms or more\n", number);
	}
	if (m_graphics.CanReportMemoryUsage()) {
		text += fmt::format(
		    "Stall {} memory: the device holds {} MB of a {} MB budget (the emulator plans for {} "
		    "MB); short by {} MB\n",
		    number, m_graphics.GetDeviceMemoryUsage() >> 20u,
		    m_graphics.GetRawMemoryBudget() >> 20u, m_graphics.GetTotalMemoryBudget() >> 20u,
		    m_graphics.MemoryShortfall() >> 20u);
	}
	if (number == FullReports) {
		text += fmt::format("Stall {}: from now on one report every 30 s; the Perf GPU line counts "
		                    "them all\n",
		                    number);
	}
	Log::WriteUnmuted(text);
}

} // namespace Libs::Graphics::StallWatch
