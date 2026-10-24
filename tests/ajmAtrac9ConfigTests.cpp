// AjmAt9NormalizeConfig: a game may pass the ATRAC9 config as the stored LE word (30 c0 72 fe).
#include "libs/ajm/atrac9_decoder.h"

#include <cstdio>
#include <cstdlib>

static void Check(bool v, const char* t) {
	if (!v) {
		std::fprintf(stderr, "AjmAtrac9ConfigTests: failed: %s\n", t);
		std::abort();
	}
}

using namespace Libs::Audio::Ajm;
#include <algorithm>
int main() {
	const uint8_t stream[4] = {0xfe, 0x72, 0xc0, 0x30};
	const uint8_t stored[4] = {0x30, 0xc0, 0x72, 0xfe};
	uint8_t       out[4];
	AjmAt9NormalizeConfig(stream, out);
	Check(std::equal(out, out + 4, stream), "stream order unchanged");
	AjmAt9NormalizeConfig(stored, out);
	Check(std::equal(out, out + 4, stream), "stored word reversed");
	Check(AjmAt9ConfigDataValid(out), "reversed config valid");
	AjmAt9NormalizeConfig(out, out);
	Check(std::equal(out, out + 4, stream), "idempotent");
	uint8_t bad[4] = {0x01, 0x02, 0x03, 0x04};
	AjmAt9NormalizeConfig(bad, out);
	Check(!AjmAt9ConfigDataValid(out), "garbage rejected");
	return 0;
}
