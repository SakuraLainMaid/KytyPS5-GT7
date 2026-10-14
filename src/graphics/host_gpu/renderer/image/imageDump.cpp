#include "graphics/host_gpu/renderer/image/imageDump.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace Libs::Graphics::ImageDump {

namespace {

struct Item {
	enum class Kind { Address, Size, Cubes, Depth, All } kind = Kind::All;
	uint64_t address                                          = 0;
	uint32_t width                                            = 0;
	uint32_t height                                           = 0;
	uint32_t layers                                           = 0; // 0: any
};

const std::vector<Item>& Items() {
	static const std::vector<Item> items = [] {
		std::vector<Item> result;
		const char*       value = std::getenv("KYTY_DUMP_IMAGES");
		if (value == nullptr) {
			return result;
		}
		const std::string_view list(value);
		size_t                 start = 0;
		while (start < list.size()) {
			const auto end  = std::min(list.find(',', start), list.size());
			auto       text = std::string(list.substr(start, end - start));
			start           = end + 1;
			text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
			if (text.empty()) {
				continue;
			}
			Item item;
			if (text == "all") {
				item.kind = Item::Kind::All;
			} else if (text == "cubes") {
				item.kind = Item::Kind::Cubes;
			} else if (text == "depth") {
				item.kind = Item::Kind::Depth;
			} else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
				item.kind    = Item::Kind::Address;
				item.address = std::strtoull(text.c_str() + 2, nullptr, 16);
			} else {
				unsigned  width = 0, height = 0, layers = 0;
				const int fields = std::sscanf(text.c_str(), "%ux%ux%u", &width, &height, &layers);
				if (fields < 2 || width == 0 || height == 0) {
					LOGF("ImageDump: KYTY_DUMP_IMAGES item \"%s\" is not understood\n",
					     text.c_str());
					continue;
				}
				item.kind   = Item::Kind::Size;
				item.width  = width;
				item.height = height;
				item.layers = fields == 3 ? layers : 0;
			}
			result.push_back(item);
		}
		LOGF("ImageDump: %zu KYTY_DUMP_IMAGES items: F9 captures write the textures they match "
		     "to image-dumps\n",
		     result.size());
		return result;
	}();
	return items;
}

uint32_t MaxCount() {
	static const uint32_t count = [] {
		const char* value = std::getenv("KYTY_DUMP_IMAGES_MAX");
		return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 40u;
	}();
	return count;
}

// GPU thread.
uint32_t                                g_capture = 0;
uint32_t                                g_count   = 0;
std::set<std::pair<uint64_t, uint64_t>> g_claimed;

float Half(uint16_t bits) {
	const uint32_t sign     = (bits >> 15u) & 1u;
	const uint32_t exponent = (bits >> 10u) & 0x1fu;
	const uint32_t mantissa = bits & 0x3ffu;
	float          value    = 0.0f;
	if (exponent == 0) {
		value = std::ldexp(static_cast<float>(mantissa), -24);
	} else if (exponent == 31) {
		value = mantissa == 0 ? std::numeric_limits<float>::infinity()
		                      : std::numeric_limits<float>::quiet_NaN();
	} else {
		value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
	}
	return sign != 0 ? -value : value;
}

// An unsigned float of `mantissa_bits` and a 5-bit exponent (the 11- and 10-bit fields).
float SmallFloat(uint32_t bits, uint32_t mantissa_bits) {
	const uint32_t exponent = (bits >> mantissa_bits) & 0x1fu;
	const uint32_t mantissa = bits & ((1u << mantissa_bits) - 1u);
	if (exponent == 0) {
		return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
	}
	if (exponent == 31) {
		return mantissa == 0 ? std::numeric_limits<float>::infinity()
		                     : std::numeric_limits<float>::quiet_NaN();
	}
	return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)),
	                  static_cast<int>(exponent) - 15 - static_cast<int>(mantissa_bits));
}

enum class Kind { None, Float, Unorm, Snorm, Uint, Depth };

struct FormatInfo {
	Kind     kind     = Kind::None;
	uint32_t channels = 0;
	uint32_t bytes    = 0;
};

FormatInfo Describe(vk::Format format) {
	using F = vk::Format;
	switch (format) {
		case F::eR8Unorm: return {Kind::Unorm, 1, 1};
		case F::eR8Snorm: return {Kind::Snorm, 1, 1};
		case F::eR8Uint: return {Kind::Uint, 1, 1};
		case F::eR8G8Unorm: return {Kind::Unorm, 2, 2};
		case F::eR8G8Snorm: return {Kind::Snorm, 2, 2};
		case F::eR8G8Uint: return {Kind::Uint, 2, 2};
		case F::eR8G8B8A8Unorm:
		case F::eR8G8B8A8Srgb:
		case F::eB8G8R8A8Unorm:
		case F::eB8G8R8A8Srgb: return {Kind::Unorm, 4, 4};
		case F::eR8G8B8A8Snorm: return {Kind::Snorm, 4, 4};
		case F::eR8G8B8A8Uint: return {Kind::Uint, 4, 4};
		case F::eA2R10G10B10UnormPack32:
		case F::eA2B10G10R10UnormPack32: return {Kind::Unorm, 4, 4};
		case F::eR16Unorm: return {Kind::Unorm, 1, 2};
		case F::eR16Snorm: return {Kind::Snorm, 1, 2};
		case F::eR16Uint: return {Kind::Uint, 1, 2};
		case F::eR16Sfloat: return {Kind::Float, 1, 2};
		case F::eR16G16Unorm: return {Kind::Unorm, 2, 4};
		case F::eR16G16Snorm: return {Kind::Snorm, 2, 4};
		case F::eR16G16Uint: return {Kind::Uint, 2, 4};
		case F::eR16G16Sfloat: return {Kind::Float, 2, 4};
		case F::eR16G16B16A16Unorm: return {Kind::Unorm, 4, 8};
		case F::eR16G16B16A16Snorm: return {Kind::Snorm, 4, 8};
		case F::eR16G16B16A16Uint: return {Kind::Uint, 4, 8};
		case F::eR16G16B16A16Sfloat: return {Kind::Float, 4, 8};
		case F::eR32Uint: return {Kind::Uint, 1, 4};
		case F::eR32Sfloat: return {Kind::Float, 1, 4};
		case F::eR32G32Uint: return {Kind::Uint, 2, 8};
		case F::eR32G32Sfloat: return {Kind::Float, 2, 8};
		case F::eR32G32B32A32Uint: return {Kind::Uint, 4, 16};
		case F::eR32G32B32A32Sfloat: return {Kind::Float, 4, 16};
		case F::eB10G11R11UfloatPack32:
		case F::eE5B9G9R9UfloatPack32: return {Kind::Float, 3, 4};
		// The depth aspect as a copy lays it out.
		case F::eD16Unorm:
		case F::eD16UnormS8Uint: return {Kind::Depth, 1, 2};
		case F::eX8D24UnormPack32:
		case F::eD24UnormS8Uint:
		case F::eD32Sfloat:
		case F::eD32SfloatS8Uint: return {Kind::Depth, 1, 4};
		default: return {};
	}
}

template <typename T>
T Load(const uint8_t* p) {
	T value {};
	std::memcpy(&value, p, sizeof(T));
	return value;
}

// One texel, its channels in RGBA order.
void Decode(vk::Format format, const uint8_t* p, float out[4]) {
	using F = vk::Format;
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	switch (format) {
		case F::eR8Unorm:
		case F::eR8G8Unorm:
		case F::eR8G8B8A8Unorm:
		case F::eR8G8B8A8Srgb:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = static_cast<float>(p[c]) / 255.0f;
			}
			return;
		case F::eB8G8R8A8Unorm:
		case F::eB8G8R8A8Srgb:
			out[0] = static_cast<float>(p[2]) / 255.0f;
			out[1] = static_cast<float>(p[1]) / 255.0f;
			out[2] = static_cast<float>(p[0]) / 255.0f;
			out[3] = static_cast<float>(p[3]) / 255.0f;
			return;
		case F::eR8Snorm:
		case F::eR8G8Snorm:
		case F::eR8G8B8A8Snorm:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = std::max(static_cast<float>(static_cast<int8_t>(p[c])) / 127.0f, -1.0f);
			}
			return;
		case F::eR8Uint:
		case F::eR8G8Uint:
		case F::eR8G8B8A8Uint:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = static_cast<float>(p[c]);
			}
			return;
		case F::eA2R10G10B10UnormPack32: {
			const auto v = Load<uint32_t>(p);
			out[0]       = static_cast<float>((v >> 20u) & 0x3ffu) / 1023.0f;
			out[1]       = static_cast<float>((v >> 10u) & 0x3ffu) / 1023.0f;
			out[2]       = static_cast<float>(v & 0x3ffu) / 1023.0f;
			out[3]       = static_cast<float>(v >> 30u) / 3.0f;
			return;
		}
		case F::eA2B10G10R10UnormPack32: {
			const auto v = Load<uint32_t>(p);
			out[0]       = static_cast<float>(v & 0x3ffu) / 1023.0f;
			out[1]       = static_cast<float>((v >> 10u) & 0x3ffu) / 1023.0f;
			out[2]       = static_cast<float>((v >> 20u) & 0x3ffu) / 1023.0f;
			out[3]       = static_cast<float>(v >> 30u) / 3.0f;
			return;
		}
		case F::eR16Unorm:
		case F::eR16G16Unorm:
		case F::eR16G16B16A16Unorm:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = static_cast<float>(Load<uint16_t>(p + 2 * c)) / 65535.0f;
			}
			return;
		case F::eR16Snorm:
		case F::eR16G16Snorm:
		case F::eR16G16B16A16Snorm:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = std::max(static_cast<float>(Load<int16_t>(p + 2 * c)) / 32767.0f, -1.0f);
			}
			return;
		case F::eR16Uint:
		case F::eR16G16Uint:
		case F::eR16G16B16A16Uint:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = static_cast<float>(Load<uint16_t>(p + 2 * c));
			}
			return;
		case F::eR16Sfloat:
		case F::eR16G16Sfloat:
		case F::eR16G16B16A16Sfloat:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = Half(Load<uint16_t>(p + 2 * c));
			}
			return;
		case F::eR32Uint:
		case F::eR32G32Uint:
		case F::eR32G32B32A32Uint:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = static_cast<float>(Load<uint32_t>(p + 4 * c));
			}
			return;
		case F::eR32Sfloat:
		case F::eR32G32Sfloat:
		case F::eR32G32B32A32Sfloat:
		case F::eD32Sfloat:
		case F::eD32SfloatS8Uint:
			for (uint32_t c = 0; c < Describe(format).channels; c++) {
				out[c] = Load<float>(p + 4 * c);
			}
			return;
		case F::eB10G11R11UfloatPack32: {
			const auto v = Load<uint32_t>(p);
			out[0]       = SmallFloat(v & 0x7ffu, 6);
			out[1]       = SmallFloat((v >> 11u) & 0x7ffu, 6);
			out[2]       = SmallFloat((v >> 22u) & 0x3ffu, 5);
			return;
		}
		case F::eE5B9G9R9UfloatPack32: {
			const auto v     = Load<uint32_t>(p);
			const int  shift = static_cast<int>(v >> 27u) - 15 - 9;
			out[0]           = std::ldexp(static_cast<float>(v & 0x1ffu), shift);
			out[1]           = std::ldexp(static_cast<float>((v >> 9u) & 0x1ffu), shift);
			out[2]           = std::ldexp(static_cast<float>((v >> 18u) & 0x1ffu), shift);
			return;
		}
		case F::eD16Unorm:
		case F::eD16UnormS8Uint: out[0] = static_cast<float>(Load<uint16_t>(p)) / 65535.0f; return;
		case F::eX8D24UnormPack32:
		case F::eD24UnormS8Uint:
			out[0] = static_cast<float>(Load<uint32_t>(p) & 0xffffffu) / 16777215.0f;
			return;
		default: return;
	}
}

void WritePng(void* context, void* data, int size) {
	auto*       out   = static_cast<std::vector<uint8_t>*>(context);
	const auto* bytes = static_cast<const uint8_t*>(data);
	out->insert(out->end(), bytes, bytes + size);
}

void Encode(const std::string& name, const std::string& description, const Layout& layout,
            const std::vector<uint8_t>& bytes) {
	const auto info = Describe(layout.format);
	if (info.kind == Kind::None || layout.tiles == 0 || layout.rows == 0 ||
	    layout.row_texels == 0) {
		return;
	}
	const uint32_t step   = std::max(layout.step, 1u);
	const uint32_t tile_w = (layout.row_texels + step - 1u) / step;
	const uint32_t tile_h = layout.rows;
	const uint32_t tiles  = layout.tiles;

	// The texels shown, decoded.
	std::vector<std::array<float, 4>> texels(static_cast<size_t>(tile_w) * tile_h * tiles);
	for (uint32_t t = 0; t < tiles; t++) {
		for (uint32_t y = 0; y < tile_h; y++) {
			const uint64_t row = t * layout.tile_pitch + y * layout.row_pitch;
			for (uint32_t x = 0; x < tile_w; x++) {
				const uint64_t at    = row + static_cast<uint64_t>(x) * step * layout.texel_bytes;
				auto&          texel = texels[(static_cast<size_t>(t) * tile_h + y) * tile_w + x];
				if (at + layout.texel_bytes <= bytes.size()) {
					Decode(layout.format, bytes.data() + at, texel.data());
				}
			}
		}
	}

	// The mapping to 0..1.
	uint64_t             not_finite = 0, negative = 0;
	float                low = std::numeric_limits<float>::max(), high = -low;
	std::array<float, 4> channel_max {};
	std::vector<float>   brightness;
	brightness.reserve(std::min<size_t>(texels.size(), 1u << 18u));
	const size_t sample_every = std::max<size_t>(1, texels.size() >> 18u);
	for (size_t i = 0; i < texels.size(); i++) {
		const auto& v      = texels[i];
		bool        finite = true;
		float       bright = 0.0f;
		for (uint32_t c = 0; c < info.channels; c++) {
			if (!std::isfinite(v[c])) {
				finite = false;
				continue;
			}
			negative += v[c] < 0.0f ? 1u : 0u;
			bright         = std::max(bright, v[c]);
			channel_max[c] = std::max(channel_max[c], v[c]);
			low            = std::min(low, v[c]);
			high           = std::max(high, v[c]);
		}
		if (!finite) {
			not_finite++;
		} else if (info.kind == Kind::Float && bright > 0.0f && i % sample_every == 0) {
			brightness.push_back(bright);
		}
	}
	float scale = 1.0f;
	if (info.kind == Kind::Float) {
		if (!brightness.empty()) {
			const size_t nth = std::min(brightness.size() - 1, brightness.size() * 99 / 100);
			std::nth_element(brightness.begin(), brightness.begin() + static_cast<ptrdiff_t>(nth),
			                 brightness.end());
			scale = brightness[nth] > 0.0f ? 1.0f / brightness[nth] : 1.0f;
		}
	}
	const auto shown = [&](float value, uint32_t c) {
		switch (info.kind) {
			case Kind::Float: return std::pow(std::clamp(value * scale, 0.0f, 1.0f), 1.0f / 2.2f);
			case Kind::Unorm: return std::clamp(value, 0.0f, 1.0f);
			case Kind::Snorm: return std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f);
			case Kind::Uint:
				return channel_max[c] > 0.0f ? std::clamp(value / channel_max[c], 0.0f, 1.0f)
				                             : 0.0f;
			case Kind::Depth:
				return high > low ? std::clamp((value - low) / (high - low), 0.0f, 1.0f) : 0.5f;
			default: return 0.0f;
		}
	};

	// The sheet: tiles side by side (a cube's six faces together), a gap between them.
	const bool           cubes     = layout.cubes && tiles % 6u == 0;
	const uint32_t       gap       = tiles > 1 ? 1u : 0u;
	const uint32_t       group     = cubes ? 6u : 1u;
	const uint32_t       group_gap = cubes ? 3u : 0u;
	const uint32_t       group_w   = group * (tile_w + gap) + group_gap;
	const uint32_t       groups    = tiles / group;
	const uint32_t       per_row = std::clamp<uint32_t>(4096u / std::max(group_w, 1u), 1u, groups);
	const uint32_t       sheet_rows = (groups + per_row - 1u) / per_row;
	const uint32_t       width      = per_row * group_w;
	const uint32_t       height     = sheet_rows * (tile_h + gap + group_gap);
	std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u, 40u);
	for (uint32_t t = 0; t < tiles; t++) {
		const uint32_t g  = t / group;
		const uint32_t ox = (g % per_row) * group_w + (t % group) * (tile_w + gap);
		const uint32_t oy = (g / per_row) * (tile_h + gap + group_gap);
		for (uint32_t y = 0; y < tile_h; y++) {
			for (uint32_t x = 0; x < tile_w; x++) {
				const auto& v      = texels[(static_cast<size_t>(t) * tile_h + y) * tile_w + x];
				auto*       out    = &rgb[(static_cast<size_t>(oy + y) * width + ox + x) * 3u];
				bool        finite = true;
				for (uint32_t c = 0; c < info.channels; c++) {
					finite = finite && std::isfinite(v[c]);
				}
				if (!finite) {
					out[0] = 255;
					out[1] = 0;
					out[2] = 255;
					continue;
				}
				for (uint32_t c = 0; c < 3; c++) {
					// One channel is grey; two are red and green.
					const uint32_t from = info.channels == 1 ? 0u : c;
					const float    value =
                        from < info.channels && from < 3 ? shown(v[from], from) : 0.0f;
					out[c] = static_cast<uint8_t>(std::lround(value * 255.0f));
				}
			}
		}
	}

	std::vector<uint8_t> png;
	png.reserve(rgb.size() / 2);
	if (stbi_write_png_to_func(&WritePng, &png, static_cast<int>(width), static_cast<int>(height),
	                           3, rgb.data(), static_cast<int>(width * 3u)) == 0) {
		LOGF("FG dump %s: the PNG could not be encoded\n", name.c_str());
		return;
	}
	std::error_code error;
	std::filesystem::create_directories("image-dumps", error);
	const auto    path = std::filesystem::path("image-dumps") / (name + ".png");
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
	const bool  written = static_cast<bool>(file);
	const char* mapping = info.kind == Kind::Float   ? "float, 99th percentile white"
	                      : info.kind == Kind::Depth ? "depth, smallest to largest"
	                      : info.kind == Kind::Uint  ? "integers against their largest"
	                      : info.kind == Kind::Snorm ? "snorm"
	                                                 : "unorm";
	LOGF("FG dump %s.png%s: %s; %u tile(s) of %ux%u (every %u texel(s)) as %ux%u; %s, scale %g, "
	     "values %g..%g; %" PRIu64 " not finite (magenta), %" PRIu64 " negative\n",
	     name.c_str(), written ? "" : " (NOT WRITTEN)", description.c_str(), tiles, tile_w, tile_h,
	     step, width, height, mapping, static_cast<double>(scale),
	     static_cast<double>(low <= high ? low : 0.0f),
	     static_cast<double>(low <= high ? high : 0.0f), not_finite, negative);
}

} // namespace

bool Enabled() {
	return !Items().empty();
}

bool Wanted(uint64_t address, uint32_t width, uint32_t height, uint32_t layers, bool depth) {
	for (const auto& item: Items()) {
		switch (item.kind) {
			case Item::Kind::All: return true;
			case Item::Kind::Cubes:
				if (layers >= 6 && layers % 6 == 0) return true;
				break;
			case Item::Kind::Depth:
				if (depth) return true;
				break;
			case Item::Kind::Address:
				if (address == item.address) return true;
				break;
			case Item::Kind::Size:
				if (width == item.width && height == item.height &&
				    (item.layers == 0 || layers == item.layers)) {
					return true;
				}
				break;
		}
	}
	return false;
}

bool Claim(uint32_t capture, uint64_t address, uint64_t content) {
	if (capture != g_capture) {
		g_capture = capture;
		g_count   = 0;
		g_claimed.clear();
	}
	if (g_count >= MaxCount()) {
		return false;
	}
	if (!g_claimed.emplace(address, content).second) {
		return false;
	}
	if (++g_count == MaxCount()) {
		LOGF("FG dump: %u images this capture, the most KYTY_DUMP_IMAGES_MAX allows\n", g_count);
	}
	return true;
}

bool ClaimTrace(uint64_t address, uint64_t content) {
	static const uint32_t most = [] {
		const char* value = std::getenv("KYTY_DUMP_IMAGES_TRACE_MAX");
		return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 24u;
	}();
	static uint32_t                                count = 0;
	static std::set<std::pair<uint64_t, uint64_t>> claimed;
	if (count >= most || !claimed.emplace(address, content).second) {
		return false;
	}
	if (++count == most) {
		LOGF("Trace image: %u pictures, the most KYTY_DUMP_IMAGES_TRACE_MAX allows\n", count);
	}
	return true;
}

uint32_t MaxSide() {
	static const uint32_t side = [] {
		const char* value = std::getenv("KYTY_DUMP_IMAGES_SIZE");
		const auto  size  = value != nullptr ? std::strtoul(value, nullptr, 10) : 1024ul;
		return static_cast<uint32_t>(std::clamp<unsigned long>(size, 64ul, 16384ul));
	}();
	return side;
}

uint64_t MaxBytes() {
	static const uint64_t bytes = [] {
		const char* value = std::getenv("KYTY_DUMP_IMAGES_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : 256ull) << 20u;
	}();
	return bytes;
}

uint32_t TexelBytes(vk::Format format, bool& depth) {
	const auto info = Describe(format);
	depth           = info.kind == Kind::Depth;
	return info.bytes;
}

void Write(std::string name, std::string description, const Layout& layout,
           std::vector<uint8_t> bytes) {
	// One decode at a time: each holds its texels as floats and its sheet.
	static std::mutex encoding;
	std::thread([name = std::move(name), description = std::move(description), layout,
	             bytes = std::move(bytes)] {
		std::scoped_lock lock {encoding};
		Encode(name, description, layout, bytes);
	}).detach();
}

} // namespace Libs::Graphics::ImageDump
