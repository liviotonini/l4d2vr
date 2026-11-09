#include <vector>
#include "../L4D2VR/wrist_hud_draw.h"
#include "test_common.h"

using namespace WristHud;

namespace
{
	bool PixelIs(const std::vector<uint8_t> &pixels, int x, int y, Color color)
	{
		const uint8_t *p = &pixels[(y * kWidth + x) * 4];
		return p[0] == color.r && p[1] == color.g && p[2] == color.b && p[3] == color.a;
	}

	int CountNot(const std::vector<uint8_t> &pixels, int x, int y, int width, int height, Color color)
	{
		int count = 0;
		for (int py = y; py < y + height; ++py)
			for (int px = x; px < x + width; ++px)
				if (!PixelIs(pixels, px, py, color))
					++count;
		return count;
	}
}

void RunWristHudTests()
{
	printf("wrist hud\n");
	std::vector<uint8_t> pixels(kWidth * kHeight * 4);

	State state;
	state.health = 100;
	Draw(state, pixels.data());
	CHECK(PixelIs(pixels, 100, 20, kHealthy));
	CHECK(PixelIs(pixels, 250, 20, kBackground));

	// Bar fills in proportion to the health, temporary health after the permanent one
	state.health = 50;
	state.temporary = 25;
	Draw(state, pixels.data());
	const int barWidth = kWidth - 100;
	CHECK(PixelIs(pixels, 92 + barWidth / 4, 20, kHealthy));
	CHECK(PixelIs(pixels, 92 + barWidth * 5 / 8, 20, kTemporary));
	CHECK(PixelIs(pixels, 92 + barWidth * 7 / 8, 20, kBarEmpty));

	// Without a gun there's nothing in the ammo area
	state.hasAmmo = false;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 8, 50, 200, 30, kBackground) == 0);

	// With a gun, the clip and the reserve are drawn
	state.hasAmmo = true;
	state.clip = 50;
	state.reserve = 360;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 8, 50, 60, 30, kBackground) > 50);
	CHECK(CountNot(pixels, 90, 57, 100, 24, kBackground) > 50);

	// Unlimited reserve: only the clip
	state.reserve = -1;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 90, 57, 100, 24, kBackground) == 0);

	// An empty clip stands out
	state.clip = 0;
	Draw(state, pixels.data());
	bool warned = false;
	for (int x = 8; x < 40 && !warned; ++x)
		for (int y = 50; y < 80 && !warned; ++y)
			warned = PixelIs(pixels, x, y, kAmmoLow);
	CHECK(warned);

	// Teammates
	state.teammateCount = 3;
	state.teammates[0] = { 'C', 80, 0, false };
	state.teammates[1] = { 'E', 30, 20, false };
	state.teammates[2] = { 'R', 200, 0, true };
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 8, 92, kWidth - 16, 24, kBackground) > 200);

	// Values far outside the range don't write outside the image
	State extreme = state;
	extreme.health = 99999;
	extreme.temporary = 99999;
	extreme.clip = 99999;
	extreme.reserve = 99999;
	extreme.teammates[0].health = -500;
	Draw(extreme, pixels.data());

	// The state comparison is what avoids uploading the same image again
	State a, b;
	a.clip = 3;
	b.clip = 3;
	CHECK(a == b);
	b.clip = 4;
	CHECK(a != b);
}
