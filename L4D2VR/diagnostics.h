#pragma once
#include <Windows.h>
#include <atomic>

// Log file, crash reports and frame timings. Safe to call from any thread.
//
// VR\vr_log.txt   what the mod did this session, written by a background thread so a frame never waits for the disk
// VR\crashes\     a minidump per crash (the last 5), with the crash also written to the log
// VR\perf.csv     one line per second with the time of each part of the frame, only with DiagnosticsEnabled=true
namespace Diagnostics
{
	enum Section
	{
		Section_Frame,      // VR::Update and the rendering of both eyes
		Section_Tracking,   // poses, head and hands
		Section_Input,      // controller actions
		Section_Submit,     // textures and overlays sent to SteamVR
		Section_Scope,      // the extra view of the sniper scope
		Section_Eyes,       // the two eyes
		Section_Models,     // bones changed by the mod (hands, other players)
		Section_Count
	};

	extern std::atomic<bool> g_PerfEnabled;

	void Init();

	// The engine installs its own handler while it starts, so this is called again once the game is running
	void InstallCrashHandler();

	void Log(const char *format, ...);

	void AddSample(Section section, float microseconds);

	// Once per rendered frame. Writes the line of perf.csv once per second.
	void EndFrame();

	class ScopedTimer
	{
	public:
		explicit ScopedTimer(Section section) : m_Section(section), m_Start{}
		{
			if (g_PerfEnabled.load(std::memory_order_relaxed))
				QueryPerformanceCounter(&m_Start);
		}

		~ScopedTimer()
		{
			if (m_Start.QuadPart == 0)
				return;

			static const double s_MicrosecondsPerTick = []
			{
				LARGE_INTEGER frequency;
				QueryPerformanceFrequency(&frequency);
				return 1000000.0 / (double)frequency.QuadPart;
			}();

			LARGE_INTEGER end;
			QueryPerformanceCounter(&end);
			AddSample(m_Section, (float)((end.QuadPart - m_Start.QuadPart) * s_MicrosecondsPerTick));
		}

		ScopedTimer(const ScopedTimer &) = delete;
		ScopedTimer &operator=(const ScopedTimer &) = delete;

	private:
		Section m_Section;
		LARGE_INTEGER m_Start;
	};
}
