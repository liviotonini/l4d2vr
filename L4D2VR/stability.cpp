#include "stability.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <thread>
#include "diagnostics.h"

namespace
{
	// The renderer's dynamic index buffer holds 32768 indices. Heavier scenes overflow it, and rendering twice per frame
	// for VR makes that more likely. Raised in the three places shaderapidx9.dll has the value hard-coded.
	const uint32_t kOriginalIndexBufferSize = 32768;
	const uint32_t kNewIndexBufferSize = 65535;

	struct IndexBufferPattern
	{
		const char *name;
		const uint8_t *bytes;
		size_t length;
		size_t valueOffset;
	};

	const uint8_t kCreatePattern[] = { 0x6A, 0x01, 0xFF, 0xD2, 0x0F, 0xB6, 0xC0, 0x50, 0x68, 0x00, 0x80, 0x00, 0x00, 0x53, 0x8B, 0xCF };
	const uint8_t kMemberPattern[] = { 0xC7, 0x86, 0xC8, 0x03, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x89 };
	const uint8_t kGetterPattern[] = { 0xB8, 0x00, 0x80, 0x00, 0x00, 0xC3 };

	const IndexBufferPattern kIndexBufferPatterns[] =
	{
		{ "create", kCreatePattern, sizeof(kCreatePattern), 9 },
		{ "member", kMemberPattern, sizeof(kMemberPattern), 6 },
		{ "getter", kGetterPattern, sizeof(kGetterPattern), 1 },
	};

	bool PatternMatches(const uint8_t *candidate, const IndexBufferPattern &pattern)
	{
		for (size_t i = 0; i < pattern.length; ++i)
		{
			// The value itself isn't compared, so running this twice doesn't fail
			if (i >= pattern.valueOffset && i < pattern.valueOffset + sizeof(uint32_t))
				continue;
			if (candidate[i] != pattern.bytes[i])
				return false;
		}
		return true;
	}

	// Patched only when the pattern is found exactly once with the expected value
	void PatchIndexBufferValue(uint8_t *codeStart, size_t codeSize, const IndexBufferPattern &pattern)
	{
		uint8_t *match = nullptr;
		int matches = 0;

		for (size_t offset = 0; offset + pattern.length <= codeSize; ++offset)
		{
			uint8_t *candidate = codeStart + offset;
			if (!PatternMatches(candidate, pattern))
				continue;

			uint32_t value;
			memcpy(&value, candidate + pattern.valueOffset, sizeof(value));
			if (value == kOriginalIndexBufferSize || value == kNewIndexBufferSize)
			{
				match = candidate;
				++matches;
			}
		}

		if (matches != 1)
		{
			Diagnostics::Log("index buffer: pattern %s found %d times, left as it was", pattern.name, matches);
			return;
		}

		uint8_t *address = match + pattern.valueOffset;
		DWORD oldProtection;
		if (!VirtualProtect(address, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtection))
			return;

		memcpy(address, &kNewIndexBufferSize, sizeof(kNewIndexBufferSize));
		VirtualProtect(address, sizeof(uint32_t), oldProtection, &oldProtection);
		FlushInstructionCache(GetCurrentProcess(), address, sizeof(uint32_t));
		Diagnostics::Log("index buffer: pattern %s raised to %u", pattern.name, kNewIndexBufferSize);
	}

	void PatchIndexBufferLimit(HMODULE module)
	{
		uint8_t *base = (uint8_t *)module;
		const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
		const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
		const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);

		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;

			for (const IndexBufferPattern &pattern : kIndexBufferPatterns)
				PatchIndexBufferValue(base + section->VirtualAddress, section->Misc.VirtualSize, pattern);
		}
	}

	void WaitForRendererAndPatch()
	{
		for (int attempt = 0; attempt < 600; ++attempt)
		{
			if (HMODULE module = GetModuleHandleA("shaderapidx9.dll"))
			{
				PatchIndexBufferLimit(module);
				return;
			}
			Sleep(100);
		}
		Diagnostics::Log("index buffer: shaderapidx9.dll wasn't loaded within 60 seconds");
	}

	// "Too many vertex format changes" is printed many times per frame on some maps, and printing to the console is slow
	typedef int(__cdecl *tSpewOutputFunc)(int type, const char *message);
	typedef tSpewOutputFunc(__cdecl *tGetSpewOutputFunc)();
	typedef tSpewOutputFunc(__cdecl *tSetSpewOutputFunc)(tSpewOutputFunc function);

	tSpewOutputFunc g_OriginalSpewOutput = nullptr;

	int __cdecl FilterSpew(int type, const char *message)
	{
		const int kSpewContinue = 1;
		if (message && strstr(message, "Too many vertex format changes"))
			return kSpewContinue;

		return g_OriginalSpewOutput ? g_OriginalSpewOutput(type, message) : kSpewContinue;
	}

	void InstallSpewFilter()
	{
		HMODULE tier0 = GetModuleHandleA("tier0.dll");
		if (!tier0)
			return;

		tSetSpewOutputFunc setSpewOutput = (tSetSpewOutputFunc)GetProcAddress(tier0, "SpewOutputFunc");
		tGetSpewOutputFunc getSpewOutput = (tGetSpewOutputFunc)GetProcAddress(tier0, "GetSpewOutputFunc");
		if (!setSpewOutput || !getSpewOutput)
			return;

		tSpewOutputFunc current = getSpewOutput();
		if (current == &FilterSpew)
			return;

		g_OriginalSpewOutput = current;
		setSpewOutput(&FilterSpew);
	}
}

namespace Stability
{
	void Init()
	{
		// winmm.dll instead of linking it: only this one function is needed
		if (HMODULE winmm = LoadLibraryA("winmm.dll"))
		{
			typedef UINT(WINAPI *tTimeBeginPeriod)(UINT);
			if (tTimeBeginPeriod timeBeginPeriod = (tTimeBeginPeriod)GetProcAddress(winmm, "timeBeginPeriod"))
				timeBeginPeriod(1);
		}

		InstallSpewFilter();
		std::thread(WaitForRendererAndPatch).detach();
	}
}
