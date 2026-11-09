#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Draws the wrist HUD into an RGBA image. No game or SteamVR types, so it can be tested on its own.
namespace WristHud
{
	const int kWidth = 256;
	const int kHeight = 128;
	const int kMaxTeammates = 3;

	struct Teammate
	{
		char letter = '?';
		int health = 0;
		int temporary = 0;
		bool incapacitated = false;

		bool operator==(const Teammate &o) const
		{
			return letter == o.letter && health == o.health && temporary == o.temporary && incapacitated == o.incapacitated;
		}
	};

	struct State
	{
		int health = 0;
		int temporary = 0;
		bool incapacitated = false;
		bool lastLife = false;   // black and white
		bool hasAmmo = false;
		int clip = 0;
		int reserve = -1;        // -1 for weapons with unlimited reserve ammo
		int teammateCount = 0;
		Teammate teammates[kMaxTeammates];

		bool operator==(const State &o) const
		{
			if (health != o.health || temporary != o.temporary || incapacitated != o.incapacitated || lastLife != o.lastLife
				|| hasAmmo != o.hasAmmo || clip != o.clip || reserve != o.reserve || teammateCount != o.teammateCount)
				return false;
			for (int i = 0; i < teammateCount; ++i)
				if (!(teammates[i] == o.teammates[i]))
					return false;
			return true;
		}
		bool operator!=(const State &o) const { return !(*this == o); }
	};

	struct Color { uint8_t r, g, b, a; };

	const Color kBackground = { 0, 0, 0, 170 };
	const Color kBarEmpty = { 60, 60, 60, 220 };
	const Color kHealthy = { 70, 200, 90, 255 };
	const Color kHurt = { 230, 190, 60, 255 };
	const Color kCritical = { 220, 70, 60, 255 };
	const Color kLastLife = { 235, 235, 235, 255 };
	const Color kTemporary = { 160, 220, 170, 255 };
	const Color kText = { 240, 240, 240, 255 };
	const Color kAmmoLow = { 235, 120, 70, 255 };

	inline void Fill(uint8_t *pixels, int x, int y, int width, int height, Color color)
	{
		const int left = std::max(x, 0), right = std::min(x + width, kWidth);
		const int top = std::max(y, 0), bottom = std::min(y + height, kHeight);
		for (int py = top; py < bottom; ++py)
		{
			uint8_t *p = pixels + (py * kWidth + left) * 4;
			for (int px = left; px < right; ++px, p += 4)
			{
				p[0] = color.r;
				p[1] = color.g;
				p[2] = color.b;
				p[3] = color.a;
			}
		}
	}

	// 5x7 font, one byte per row
	inline const uint8_t *Glyph(char c)
	{
		static const uint8_t kDigits[10][7] =
		{
			{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
			{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
			{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
			{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
			{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
		};
		static const uint8_t kSlash[7] = { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 };
		static const uint8_t kB[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
		static const uint8_t kC[7] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E };
		static const uint8_t kE[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
		static const uint8_t kF[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 };
		static const uint8_t kL[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
		static const uint8_t kN[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 };
		static const uint8_t kR[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
		static const uint8_t kZ[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F };
		static const uint8_t kQuestion[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };

		if (c >= '0' && c <= '9')
			return kDigits[c - '0'];
		switch (c)
		{
		case '/': return kSlash;
		case 'B': return kB;
		case 'C': return kC;
		case 'E': return kE;
		case 'F': return kF;
		case 'L': return kL;
		case 'N': return kN;
		case 'R': return kR;
		case 'Z': return kZ;
		default: return kQuestion;
		}
	}

	inline int TextWidth(const char *text, int scale)
	{
		return (int)strlen(text) * 6 * scale - scale;
	}

	inline void DrawText(uint8_t *pixels, int x, int y, const char *text, int scale, Color color)
	{
		for (; *text; ++text, x += 6 * scale)
		{
			if (*text == ' ')
				continue;

			const uint8_t *glyph = Glyph(*text);
			for (int row = 0; row < 7; ++row)
				for (int column = 0; column < 5; ++column)
					if (glyph[row] & (0x10 >> column))
						Fill(pixels, x + column * scale, y + row * scale, scale, scale, color);
		}
	}

	inline Color HealthColor(int health, bool lastLife)
	{
		if (lastLife)
			return kLastLife;
		if (health >= 40)
			return kHealthy;
		return health >= 25 ? kHurt : kCritical;
	}

	inline void DrawBar(uint8_t *pixels, int x, int y, int width, int height, int health, int temporary, bool lastLife, bool incapacitated, int maxHealth)
	{
		Fill(pixels, x, y, width, height, kBarEmpty);

		const int healthWidth = std::clamp(health * width / maxHealth, 0, width);
		const int temporaryWidth = std::clamp(temporary * width / maxHealth, 0, width - healthWidth);
		Fill(pixels, x, y, healthWidth, height, incapacitated ? kCritical : HealthColor(health + temporary, lastLife));
		Fill(pixels, x + healthWidth, y, temporaryWidth, height, kTemporary);
	}

	inline void Draw(const State &state, uint8_t *pixels)
	{
		Fill(pixels, 0, 0, kWidth, kHeight, kBackground);

		char text[32];

		// Health: the number is what the game adds up to decide when you limp
		const int total = std::max(state.health, 0) + std::max(state.temporary, 0);
		snprintf(text, sizeof(text), "%d", std::min(total, 999));
		const int maxHealth = state.incapacitated ? 300 : 100;
		DrawText(pixels, 8, 8, text, 4, HealthColor(total, state.lastLife));
		DrawBar(pixels, 92, 12, kWidth - 100, 20, state.health, state.temporary, state.lastLife, state.incapacitated, maxHealth);

		if (state.hasAmmo)
		{
			snprintf(text, sizeof(text), "%d", std::min(state.clip, 999));
			DrawText(pixels, 8, 50, text, 4, state.clip == 0 ? kAmmoLow : kText);

			if (state.reserve >= 0)
			{
				snprintf(text, sizeof(text), "/%d", std::min(state.reserve, 9999));
				DrawText(pixels, 8 + TextWidth("000", 4) + 8, 57, text, 3, state.reserve == 0 ? kAmmoLow : kText);
			}
		}

		const int columnWidth = (kWidth - 8) / kMaxTeammates;
		for (int i = 0; i < std::min(state.teammateCount, kMaxTeammates); ++i)
		{
			const Teammate &mate = state.teammates[i];
			const int x = 8 + i * columnWidth;
			const char letter[2] = { mate.letter, 0 };
			DrawText(pixels, x, 92, letter, 3, kText);
			DrawBar(pixels, x + 22, 98, columnWidth - 34, 12, mate.health, mate.temporary, false, mate.incapacitated, mate.incapacitated ? 300 : 100);
		}
	}
}
