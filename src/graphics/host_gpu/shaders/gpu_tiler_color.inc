// ColorTransform values match imageInfo.h.

// An unsigned float of a 5-bit exponent and `mantissa_bits` narrowed by one mantissa bit
// (truncated); a NaN keeps a mantissa bit set (imageInfo.h's NarrowSmallFloat).
uint narrow_small_float(uint bits, uint mantissa_bits) {
	uint narrow = bits >> 1u;
	uint mask   = (1u << mantissa_bits) - 1u;
	if ((bits >> mantissa_bits) == 0x1fu && (bits & mask) != 0u && (narrow & (mask >> 1u)) == 0u) {
		narrow |= 1u;
	}
	return narrow;
}

uvec2 transform_color(uvec2 value, uint transform, bool to_host) {
	switch (transform) {
		case 1u:
			return uvec2((value.y & 0xffffu) | (value.x & 0xffff0000u),
			             (value.x & 0xffffu) | (value.y & 0xffff0000u));
		case 2u: {
			// The guest's 10_11_11: red in the low 10 bits, green in the next 11, blue in the top
			// 11; the host's B10G11R11: red in the low 11, green in the next 11, blue in the top
			// 10. Each component keeps its place (Guest10_11_11ToHost, Host10_11_11ToGuest).
			uint packed = value.x;
			if (to_host) {
				value.x = ((packed & 0x3ffu) << 1u) | (((packed >> 10u) & 0x7ffu) << 11u) |
				          (narrow_small_float(packed >> 21u, 6u) << 22u);
			} else {
				value.x = narrow_small_float(packed & 0x7ffu, 6u) |
				          (((packed >> 11u) & 0x7ffu) << 10u) | (((packed >> 22u) << 1u) << 21u);
			}
			break;
		}
	}
	return value;
}
