#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Draws the wrist HUD into an RGBA image. No game or SteamVR types, so it can be tested on its own.
// Same look as the HUD of the Update mod: dark panel with cyan brackets, seven segment numbers and thin bars.
namespace WristHud
{
	const int kWidth = 256;
	const int kHeight = 128;
	const int kMaxTeammates = 3;
	const int kMaxItems = 3;

	struct Teammate
	{
		char name[24] = {};
		int health = 0;
		int temporary = 0;
		bool incapacitated = false;
		bool ledge = false;
		bool lastLife = false;
		bool controlled = false; // held by a special infected

		bool operator==(const Teammate &o) const
		{
			return strcmp(name, o.name) == 0 && health == o.health && temporary == o.temporary && incapacitated == o.incapacitated
				&& ledge == o.ledge && lastLife == o.lastLife && controlled == o.controlled;
		}
	};

	struct State
	{
		int health = 0;
		int temporary = 0;
		bool incapacitated = false;
		bool ledge = false;
		bool lastLife = false;
		bool hasAmmo = false;
		int clip = 0;
		int reserve = -1;      // -1 for weapons with unlimited reserve ammo
		int clipLowAt = 0;     // at or below this the number turns red
		int reserveLowAt = 0;
		int leftClip = -1;     // the left hand's gun, -1 without one
		int leftClipLowAt = 0;
		int teammateCount = 0;
		Teammate teammates[kMaxTeammates];
		int itemCount = 0;
		char items[kMaxItems][4] = {};
		int commonKills = 0;
		int specialKills = 0;

		bool operator==(const State &o) const
		{
			if (health != o.health || temporary != o.temporary || incapacitated != o.incapacitated || ledge != o.ledge || lastLife != o.lastLife
				|| hasAmmo != o.hasAmmo || clip != o.clip || reserve != o.reserve || clipLowAt != o.clipLowAt || reserveLowAt != o.reserveLowAt
				|| leftClip != o.leftClip || leftClipLowAt != o.leftClipLowAt
				|| teammateCount != o.teammateCount || itemCount != o.itemCount || commonKills != o.commonKills || specialKills != o.specialKills)
				return false;
			for (int i = 0; i < teammateCount; ++i)
				if (!(teammates[i] == o.teammates[i]))
					return false;
			for (int i = 0; i < itemCount; ++i)
				if (strcmp(items[i], o.items[i]) != 0)
					return false;
			return true;
		}
		bool operator!=(const State &o) const { return !(*this == o); }
	};

	struct Color { uint8_t r, g, b, a; };

	const uint8_t kPanelAlpha = 128; // only the panel is see-through, the numbers aren't
	const Color kPanel = { 8, 10, 14, kPanelAlpha };
	const Color kBrackets = { 60, 220, 255, 220 };
	const Color kInnerFrame = { 20, 60, 70, kPanelAlpha };
	const Color kBarFrame = { 60, 60, 60, 190 };
	const Color kHealthy = { 60, 220, 255, 255 };
	const Color kHurt = { 255, 220, 60, 255 };
	const Color kCritical = { 255, 60, 60, 255 };
	const Color kTemporary = { 60, 255, 120, 255 };
	const Color kText = { 240, 240, 240, 255 };
	const Color kAmmo = { 240, 240, 240, 255 };
	const Color kReserve = { 200, 200, 200, 230 };
	const Color kLow = { 255, 80, 80, 255 };
	const Color kLedge = { 244, 164, 96, 230 };
	const Color kControlled = { 160, 32, 240, 230 };
	const Color kLastLife = { 248, 248, 255, 230 };

	inline Color WithAlpha(Color color, uint8_t alpha)
	{
		color.a = alpha;
		return color;
	}

	inline Color HealthColor(int health, uint8_t alpha = 255)
	{
		if (health < 15)
			return WithAlpha(kCritical, alpha);
		return WithAlpha(health < 40 ? kHurt : kHealthy, alpha);
	}

	inline Color DownHealthColor(int health, uint8_t alpha = 255)
	{
		return WithAlpha(health <= 30 ? kCritical : kHurt, alpha);
	}

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

	inline void Frame(uint8_t *pixels, int x, int y, int width, int height, Color color)
	{
		Fill(pixels, x, y, width, 1, color);
		Fill(pixels, x, y + height - 1, width, 1, color);
		Fill(pixels, x, y, 1, height, color);
		Fill(pixels, x + width - 1, y, 1, height, color);
	}

	inline void CornerBrackets(uint8_t *pixels, int x, int y, int width, int height, Color color)
	{
		const int t = 2, length = 10;
		Fill(pixels, x, y, length, t, color);
		Fill(pixels, x, y, t, length, color);
		Fill(pixels, x + width - length, y, length, t, color);
		Fill(pixels, x + width - t, y, t, length, color);
		Fill(pixels, x, y + height - t, length, t, color);
		Fill(pixels, x, y + height - length, t, length, color);
		Fill(pixels, x + width - length, y + height - t, length, t, color);
		Fill(pixels, x + width - t, y + height - length, t, length, color);
	}

	struct SevenSegment
	{
		int length;
		int thickness;
		int digitGap;

		int DigitWidth() const { return length + 2 * thickness; }
		int Height() const { return 2 * length + 3 * thickness; }
	};

	const SevenSegment kLarge = { 12, 3, 4 };
	const SevenSegment kSmall = { 8, 2, 3 };

	inline void DrawDigit(uint8_t *pixels, int x, int y, int digit, const SevenSegment &style, Color color)
	{
		// Bits: top, upper right, lower right, bottom, lower left, upper left, middle
		static const uint8_t kSegments[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
		const uint8_t mask = kSegments[digit];
		const int L = style.length, T = style.thickness;
		const int x1 = x + T + L, y1 = y + T + L, y2 = y + 2 * T + 2 * L;

		if (mask & 0x01) Fill(pixels, x + T, y, L, T, color);
		if (mask & 0x02) Fill(pixels, x1, y + T, T, L, color);
		if (mask & 0x04) Fill(pixels, x1, y1 + T, T, L, color);
		if (mask & 0x08) Fill(pixels, x + T, y2, L, T, color);
		if (mask & 0x10) Fill(pixels, x, y1 + T, T, L, color);
		if (mask & 0x20) Fill(pixels, x, y + T, T, L, color);
		if (mask & 0x40) Fill(pixels, x + T, y1, L, T, color);
	}

	// Returns the width drawn
	inline int DrawNumber(uint8_t *pixels, int x, int y, int value, const SevenSegment &style, Color color)
	{
		char text[16];
		snprintf(text, sizeof(text), "%d", std::max(value, 0));

		int pen = x;
		for (const char *p = text; *p; ++p)
		{
			DrawDigit(pixels, pen, y, *p - '0', style, color);
			pen += style.DigitWidth() + style.digitGap;
		}
		return pen - x;
	}

	// 5x7 letters and digits, one byte per row
	inline const uint8_t *Glyph(char c)
	{
		static const uint8_t kQuestion[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };
		static const uint8_t kSlash[7] = { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 };
		static const uint8_t kDigits[10][7] =
		{
			{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
			{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
			{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
			{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
			{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
		};
		static const uint8_t kLetters[26][7] =
		{
			{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
			{ 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F }, { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
			{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
			{ 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
			{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F }, { 0x1F, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C },
			{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
			{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
			{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
			{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
			{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
			{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
			{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
			{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
		};

		if (c >= '0' && c <= '9')
			return kDigits[c - '0'];
		if (c >= 'A' && c <= 'Z')
			return kLetters[c - 'A'];
		if (c == '/')
			return kSlash;
		return kQuestion;
	}

	inline void DrawBlockText(uint8_t *pixels, int x, int y, const char *text, int scale, Color color)
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

	// Smooth text with a dark outline, for names and small numbers. Names can have any character.
	inline void DrawSmoothText(uint8_t *pixels, int x, int y, int maxWidth, const char *utf8, int fontPixels, Color color)
	{
		wchar_t text[64];
		if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text, 64))
			return;

		const int width = std::min(maxWidth, kWidth - x), height = std::min(fontPixels + 8, kHeight - y);
		if (width <= 2 || height <= 2 || x < 0 || y < 0)
			return;

		HDC dc = CreateCompatibleDC(nullptr);
		if (!dc)
			return;

		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;

		void *bits = nullptr;
		HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		HFONT font = CreateFontW(-fontPixels, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

		if (bitmap && font && bits)
		{
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			HGDIOBJ oldFont = SelectObject(dc, font);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(255, 255, 255));
			TextOutW(dc, 1, 1, text, lstrlenW(text));
			GdiFlush();

			const uint8_t *cover = (const uint8_t *)bits;
			const auto at = [&](int px, int py) { return (px < 0 || py < 0 || px >= width || py >= height) ? 0 : (int)cover[(py * width + px) * 4 + 1]; };

			for (int py = 0; py < height; ++py)
			{
				for (int px = 0; px < width; ++px)
				{
					const int amount = at(px, py);
					const int outline = std::max({ at(px - 1, py), at(px + 1, py), at(px, py - 1), at(px, py + 1) });
					if (amount == 0 && outline == 0)
						continue;

					uint8_t *p = pixels + ((y + py) * kWidth + x + px) * 4;
					// The outline first, then the letter on top
					for (int channel = 0; channel < 3; ++channel)
						p[channel] = (uint8_t)(p[channel] * (255 - outline) / 255);
					p[3] = (uint8_t)std::max<int>(p[3], outline);

					const uint8_t source[3] = { color.r, color.g, color.b };
					for (int channel = 0; channel < 3; ++channel)
						p[channel] = (uint8_t)((p[channel] * (255 - amount) + source[channel] * amount) / 255);
					p[3] = (uint8_t)std::max<int>(p[3], amount * color.a / 255);
				}
			}

			SelectObject(dc, oldFont);
			SelectObject(dc, oldBitmap);
		}

		if (font)
			DeleteObject(font);
		if (bitmap)
			DeleteObject(bitmap);
		DeleteDC(dc);
	}

	inline void DrawTeammate(uint8_t *pixels, int row, const Teammate &mate)
	{
		const int barW = 62, barH = 10;
		const int barX = kWidth - 10 - barW;
		const int barY = 18 + row * 24;
		const int nameX = 124;

		char upper[sizeof(mate.name)];
		snprintf(upper, sizeof(upper), "%s", mate.name);
		for (char *c = upper; *c; ++c)
			if (*c >= 'a' && *c <= 'z')
				*c = (char)(*c - 32);
		DrawSmoothText(pixels, nameX, barY + (barH - 12) / 2 - 1, barX - nameX - 4, upper, 12, kText);

		Frame(pixels, barX, barY, barW, barH, kBarFrame);
		const int innerW = barW - 2, innerH = barH - 2;

		const bool down = mate.incapacitated || mate.ledge;
		const int percent = std::clamp(down ? mate.health * 100 / 300 : mate.health, 0, 100);
		const int permanentW = innerW * percent / 100;

		Color color = down ? DownHealthColor(percent, 230) : HealthColor(percent, 230);
		if (mate.ledge)
			color = kLedge;
		else if (!mate.incapacitated && mate.controlled)
			color = kControlled;
		else if (!mate.incapacitated && mate.lastLife)
			color = kLastLife;

		Fill(pixels, barX + 1, barY + 1, permanentW, innerH, color);

		if (!down)
		{
			const int temporaryW = std::min(innerW - permanentW, innerW * std::clamp(mate.temporary, 0, 100) / 100);
			Fill(pixels, barX + 1 + permanentW, barY + 1, temporaryW, innerH, WithAlpha(kTemporary, 210));
		}
	}

	inline void Draw(const State &state, uint8_t *pixels)
	{
		Fill(pixels, 0, 0, kWidth, kHeight, kPanel);
		CornerBrackets(pixels, 2, 2, kWidth - 4, kHeight - 4, kBrackets);
		Frame(pixels, 8, 8, kWidth - 16, kHeight - 16, kInnerFrame);

		char text[32];

		const bool down = state.incapacitated || state.ledge;
		const Color healthColor = down ? DownHealthColor(state.health) : (state.lastLife ? kLastLife : HealthColor(state.health));
		const int healthW = DrawNumber(pixels, 18, 18, std::min(std::max(state.health, 0), 999), kLarge, healthColor);

		if (state.temporary > 0)
		{
			snprintf(text, sizeof(text), "+%d", std::min(state.temporary, 999));
			DrawSmoothText(pixels, 18 + healthW + 4, 20, 60, text, 16, kTemporary);
		}

		for (int i = 0; i < std::min(state.teammateCount, kMaxTeammates); ++i)
			DrawTeammate(pixels, i, state.teammates[i]);

		if (state.hasAmmo)
		{
			const int y = 57;
			int x = 18;
			const bool clipLow = state.clip <= state.clipLowAt;
			x += DrawNumber(pixels, x, y, std::min(state.clip, 999), kLarge, clipLow ? kLow : kAmmo);

			const int smallY = y + kLarge.Height() - kSmall.Height();
			DrawBlockText(pixels, x, smallY + 2, "/", 2, kReserve);
			x += 14;

			if (state.reserve < 0)
				DrawSmoothText(pixels, x, smallY - 8, 40, "\xE2\x88\x9E", 28, kText);
			else
				DrawNumber(pixels, x, smallY, std::min(state.reserve, 999), kSmall, state.reserve <= state.reserveLowAt ? WithAlpha(kLow, 230) : kReserve);
		}

		// The items, closer together with the left hand's gun after them. Block letters of 12 pixels at this size.
		const int itemsY = 97, itemStep = state.leftClip >= 0 ? 42 : 48, letterW = 12;
		const int itemCount = std::min(state.itemCount, kMaxItems);
		int itemsEnd = 18;
		for (int i = 0; i < itemCount; ++i)
		{
			DrawBlockText(pixels, 18 + i * itemStep, itemsY, state.items[i], 2, kText);
			itemsEnd = 18 + i * itemStep + (int)strlen(state.items[i]) * letterW;
		}
		if (state.leftClip >= 0)
		{
			snprintf(text, sizeof(text), "L%d", std::min(state.leftClip, 999));
			const int x = 18 + itemCount * itemStep;
			DrawBlockText(pixels, x, itemsY, text, 2, state.leftClip <= state.leftClipLowAt ? kLow : kAmmo);
			itemsEnd = x + (int)strlen(text) * letterW;
		}

		// The kills, if there's room left
		snprintf(text, sizeof(text), "%d/%d", std::max(state.commonKills, 0), std::max(state.specialKills, 0));
		const int estimatedW = (int)strlen(text) * 10 + 6;
		if (kWidth - 18 - estimatedW >= itemsEnd)
			DrawSmoothText(pixels, kWidth - 18 - estimatedW, itemsY - 4, estimatedW + 6, text, 16, kText);
	}
}
