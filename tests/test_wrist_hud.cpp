#include <cstdio>
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

	bool HasPixel(const std::vector<uint8_t> &pixels, int x, int y, int width, int height, Color color)
	{
		for (int py = y; py < y + height; ++py)
			for (int px = x; px < x + width; ++px)
				if (PixelIs(pixels, px, py, color))
					return true;
		return false;
	}
}

void RunWristHudTests()
{
	printf("wrist hud\n");
	std::vector<uint8_t> pixels(kWidth * kHeight * 4);

	State state;
	state.health = 100;
	Draw(state, pixels.data());

	// The panel: see-through background, opaque brackets in the corners
	CHECK(PixelIs(pixels, 120, 60, kPanel));
	CHECK(PixelIs(pixels, 3, 3, kBrackets));
	CHECK(PixelIs(pixels, kWidth - 4, kHeight - 4, kBrackets));

	// Health in the colors of the Update's HUD
	CHECK(HasPixel(pixels, 18, 18, 62, 33, kHealthy));
	state.health = 30;
	Draw(state, pixels.data());
	CHECK(HasPixel(pixels, 18, 18, 62, 33, kHurt));
	CHECK(!HasPixel(pixels, 18, 18, 62, 33, kHealthy));
	state.health = 10;
	Draw(state, pixels.data());
	CHECK(HasPixel(pixels, 18, 18, 62, 33, kCritical));

	// Temporary health, next to the number
	state.health = 50;
	state.temporary = 25;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 44, 18, 60, 26, kPanel) > 80);

	// Without a gun there's nothing in the ammo area
	state.hasAmmo = false;
	state.temporary = 0;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 18, 57, 100, 33, kPanel) == 0);

	// With a gun, the clip and the reserve
	state.hasAmmo = true;
	state.clip = 50;
	state.reserve = 360;
	state.clipLowAt = 17;
	state.reserveLowAt = 130;
	Draw(state, pixels.data());
	CHECK(HasPixel(pixels, 18, 57, 44, 33, kAmmo));
	CHECK(HasPixel(pixels, 70, 57, 50, 33, kReserve));

	// Low numbers turn red
	state.clip = 3;
	state.reserve = 20;
	Draw(state, pixels.data());
	CHECK(HasPixel(pixels, 18, 57, 44, 33, kLow));
	CHECK(HasPixel(pixels, 60, 57, 60, 33, WithAlpha(kLow, 230)));

	// A pistol's reserve is unlimited: not a number, the infinity sign
	state.clip = 15;
	state.reserve = -1;
	Draw(state, pixels.data());
	CHECK(HasPixel(pixels, 60, 57, 60, 33, kText));
	CHECK(!HasPixel(pixels, 60, 57, 60, 33, kReserve) || HasPixel(pixels, 60, 57, 60, 33, kText));

	// Teammates with names and bars, including names that aren't plain ASCII
	state.teammateCount = 3;
	snprintf(state.teammates[0].name, sizeof(state.teammates[0].name), "Coach");
	state.teammates[0].health = 80;
	snprintf(state.teammates[1].name, sizeof(state.teammates[1].name), "\xC3\x89llis \xE6\x97\xA5\xE6\x9C\xAC");
	state.teammates[1].health = 30;
	state.teammates[1].temporary = 20;
	snprintf(state.teammates[2].name, sizeof(state.teammates[2].name), "Rochelle has a very long name indeed");
	state.teammates[2].health = 200;
	state.teammates[2].incapacitated = true;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 124, 16, 54, 16, kPanel) > 10); // the name was drawn
	CHECK(HasPixel(pixels, 186, 20, 60, 8, HealthColor(80, 230)));
	CHECK(HasPixel(pixels, 186, 44, 60, 8, HealthColor(30, 230)));
	CHECK(HasPixel(pixels, 186, 44, 60, 8, WithAlpha(kTemporary, 210)));
	CHECK(HasPixel(pixels, 186, 68, 60, 8, DownHealthColor(66, 230)));

	// Items and kills along the bottom
	state.itemCount = 2;
	snprintf(state.items[0], sizeof(state.items[0]), "MOL");
	snprintf(state.items[1], sizeof(state.items[1]), "PIL");
	state.commonKills = 123;
	state.specialKills = 4;
	Draw(state, pixels.data());
	CHECK(CountNot(pixels, 18, 97, 40, 14, kPanel) > 30);
	CHECK(CountNot(pixels, 66, 97, 40, 14, kPanel) > 30);
	CHECK(CountNot(pixels, 170, 93, 70, 22, kPanel) > 30);

	// Values far outside the range don't write outside the image
	State extreme = state;
	extreme.health = 99999;
	extreme.temporary = 99999;
	extreme.clip = 99999;
	extreme.reserve = 99999;
	extreme.commonKills = 2000000000;
	extreme.specialKills = 2000000000;
	extreme.teammates[0].health = -500;
	extreme.teammates[1].temporary = 5000;
	Draw(extreme, pixels.data());

	// The state comparison is what avoids uploading the same image again
	State a, b;
	a.clip = 3;
	b.clip = 3;
	CHECK(a == b);
	b.clip = 4;
	CHECK(a != b);
	b.clip = 3;
	b.teammateCount = 1;
	a.teammateCount = 1;
	snprintf(a.teammates[0].name, sizeof(a.teammates[0].name), "A");
	snprintf(b.teammates[0].name, sizeof(b.teammates[0].name), "B");
	CHECK(a != b);
}
