#include "diagnostics.h"
#include <DbgHelp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "openvr.h"

namespace Diagnostics
{
	std::atomic<bool> g_PerfEnabled{ false };
}

namespace
{
	const char *const kLogPath = "VR\\vr_log.txt";
	const char *const kPerfPath = "VR\\perf.csv";
	const char *const kCrashDirectory = "VR\\crashes";
	const int kCrashDumpsKept = 5;
	const size_t kMaxQueuedLines = 4000;

	const char *const kSectionNames[Diagnostics::Section_Count] =
	{
		"frame", "tracking", "input", "submit", "scope", "eyes", "models"
	};

	struct QueuedLine
	{
		bool perf;
		std::string text;
	};

	std::mutex g_QueueMutex;
	std::condition_variable g_QueueChanged;
	std::deque<QueuedLine> g_Queue;
	bool g_LogWriterStarted = false;

	// The log and perf files are only open in the writer thread, and in the crash handler when that thread is stuck
	FILE *g_LogFile = nullptr;
	FILE *g_PerfFile = nullptr;

	LPTOP_LEVEL_EXCEPTION_FILTER g_PreviousCrashHandler = nullptr;
	HMODULE g_DbgHelp = nullptr;

	typedef BOOL(WINAPI *tMiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
	tMiniDumpWriteDump g_MiniDumpWriteDump = nullptr;

	void WriteLine(const QueuedLine &line)
	{
		FILE *&file = line.perf ? g_PerfFile : g_LogFile;
		if (!file)
			fopen_s(&file, line.perf ? kPerfPath : kLogPath, "w");
		if (!file)
			return;

		fputs(line.text.c_str(), file);
		fputc('\n', file);
	}

	void LogWriterThread()
	{
		std::unique_lock<std::mutex> lock(g_QueueMutex);
		while (true)
		{
			g_QueueChanged.wait(lock, [] { return !g_Queue.empty(); });

			std::deque<QueuedLine> batch;
			batch.swap(g_Queue);
			lock.unlock();

			for (const QueuedLine &line : batch)
				WriteLine(line);

			// Flushed once per batch: if the game crashes, what was logged before is on the disk
			if (g_LogFile)
				fflush(g_LogFile);
			if (g_PerfFile)
				fflush(g_PerfFile);

			lock.lock();
		}
	}

	void Enqueue(bool perf, std::string text)
	{
		{
			std::lock_guard<std::mutex> lock(g_QueueMutex);
			if (g_Queue.size() >= kMaxQueuedLines)
				return;
			g_Queue.push_back({ perf, std::move(text) });
		}
		g_QueueChanged.notify_one();
	}

	// Writes the lines still in the queue. The thread that crashed may be the one holding the lock, so it doesn't wait.
	void FlushQueueAfterCrash()
	{
		std::unique_lock<std::mutex> lock(g_QueueMutex, std::defer_lock);
		for (int attempt = 0; attempt < 20 && !lock.try_lock(); ++attempt)
			Sleep(5);
		if (!lock.owns_lock())
			return;

		for (const QueuedLine &line : g_Queue)
			WriteLine(line);
		g_Queue.clear();

		if (g_LogFile)
			fflush(g_LogFile);
		if (g_PerfFile)
			fflush(g_PerfFile);
	}

	void PruneCrashDumps()
	{
		try
		{
			std::vector<std::filesystem::directory_entry> dumps;
			for (const auto &entry : std::filesystem::directory_iterator(kCrashDirectory))
			{
				if (entry.path().extension() == ".dmp")
					dumps.push_back(entry);
			}

			if ((int)dumps.size() < kCrashDumpsKept)
				return;

			std::sort(dumps.begin(), dumps.end(), [](const auto &a, const auto &b) { return a.last_write_time() > b.last_write_time(); });
			// One less than the limit, the next crash adds its own
			for (size_t i = kCrashDumpsKept - 1; i < dumps.size(); ++i)
				std::filesystem::remove(dumps[i].path());
		}
		catch (const std::filesystem::filesystem_error &) {}
	}

	LONG WINAPI CrashHandler(EXCEPTION_POINTERS *exception)
	{
		static LONG s_Entered = 0;
		if (InterlockedExchange(&s_Entered, 1) == 0)
		{
			const EXCEPTION_RECORD *record = exception->ExceptionRecord;

			char moduleName[MAX_PATH] = "unknown";
			uintptr_t moduleBase = 0;
			HMODULE module = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)record->ExceptionAddress, &module))
			{
				GetModuleFileNameA(module, moduleName, sizeof(moduleName));
				moduleBase = (uintptr_t)module;
			}

			char line[MAX_PATH + 128];
			snprintf(line, sizeof(line), "CRASH: exception 0x%08X at %p (%s + 0x%X)", (unsigned)record->ExceptionCode, record->ExceptionAddress,
				moduleName, (unsigned)((uintptr_t)record->ExceptionAddress - moduleBase));
			Enqueue(false, line);
			FlushQueueAfterCrash();

			if (g_MiniDumpWriteDump)
			{
				SYSTEMTIME time;
				GetLocalTime(&time);
				char path[MAX_PATH];
				snprintf(path, sizeof(path), "%s\\crash_%04d%02d%02d_%02d%02d%02d.dmp", kCrashDirectory, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);

				HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE)
				{
					MINIDUMP_EXCEPTION_INFORMATION info;
					info.ThreadId = GetCurrentThreadId();
					info.ExceptionPointers = exception;
					info.ClientPointers = FALSE;
					g_MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
						(MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), &info, nullptr, nullptr);
					CloseHandle(file);
				}
			}
		}

		return g_PreviousCrashHandler ? g_PreviousCrashHandler(exception) : EXCEPTION_CONTINUE_SEARCH;
	}

	// Timings of the current second
	std::mutex g_PerfMutex;
	std::array<std::vector<float>, Diagnostics::Section_Count> g_PerfSamples;
	bool g_PerfHeaderWritten = false;
	std::chrono::steady_clock::time_point g_PerfWindowStart;
	int g_PerfFramesInWindow = 0;
	uint32_t g_PerfLastDropped = 0;
	uint32_t g_PerfLastReprojected = 0;
	bool g_PerfHasCompositorCounters = false;

	void AppendSectionStats(std::string &row, std::vector<float> &samples)
	{
		char buffer[64];
		if (samples.empty())
		{
			row += ",0,0,0";
			return;
		}

		float total = 0.0f;
		for (float sample : samples)
			total += sample;

		std::sort(samples.begin(), samples.end());
		const float p95 = samples[std::min(samples.size() - 1, samples.size() * 95 / 100)];
		const float p99 = samples[std::min(samples.size() - 1, samples.size() * 99 / 100)];

		snprintf(buffer, sizeof(buffer), ",%.0f,%.0f,%.0f", total / samples.size(), p95, p99);
		row += buffer;
	}
}

namespace Diagnostics
{
	void Init()
	{
		CreateDirectoryA(kCrashDirectory, nullptr);
		PruneCrashDumps();

		// Loaded now because a crash handler shouldn't have to load anything
		g_DbgHelp = LoadLibraryA("dbghelp.dll");
		if (g_DbgHelp)
			g_MiniDumpWriteDump = (tMiniDumpWriteDump)GetProcAddress(g_DbgHelp, "MiniDumpWriteDump");

		{
			std::lock_guard<std::mutex> lock(g_QueueMutex);
			if (!g_LogWriterStarted)
			{
				g_LogWriterStarted = true;
				std::thread(LogWriterThread).detach();
			}
		}

		InstallCrashHandler();
	}

	void InstallCrashHandler()
	{
		LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(CrashHandler);
		if (previous != CrashHandler)
			g_PreviousCrashHandler = previous;
	}

	void Log(const char *format, ...)
	{
		char line[1024];
		SYSTEMTIME time;
		GetLocalTime(&time);
		int length = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

		va_list args;
		va_start(args, format);
		vsnprintf(line + length, sizeof(line) - length, format, args);
		va_end(args);

		Enqueue(false, line);
	}

	void AddSample(Section section, float microseconds)
	{
		std::lock_guard<std::mutex> lock(g_PerfMutex);
		g_PerfSamples[section].push_back(microseconds);
	}

	void EndFrame()
	{
		if (!g_PerfEnabled.load(std::memory_order_relaxed))
		{
			g_PerfFramesInWindow = 0;
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (g_PerfFramesInWindow == 0)
			g_PerfWindowStart = now;
		++g_PerfFramesInWindow;

		const float elapsed = std::chrono::duration<float>(now - g_PerfWindowStart).count();
		if (elapsed < 1.0f)
			return;

		std::string row;
		if (!g_PerfHeaderWritten)
		{
			g_PerfHeaderWritten = true;
			row = "time,fps,dropped,reprojected,memory_mb";
			for (const char *name : kSectionNames)
			{
				row += std::string(",") + name + "_avg_us,";
				row += std::string(name) + "_p95_us,";
				row += std::string(name) + "_p99_us";
			}
			Enqueue(true, row);
		}

		uint32_t dropped = 0, reprojected = 0;
		if (vr::VRCompositor())
		{
			vr::Compositor_CumulativeStats stats = {};
			vr::VRCompositor()->GetCumulativeStats(&stats, sizeof(stats));
			if (g_PerfHasCompositorCounters)
			{
				dropped = stats.m_nNumDroppedFrames - g_PerfLastDropped;
				reprojected = stats.m_nNumReprojectedFrames - g_PerfLastReprojected;
			}
			g_PerfLastDropped = stats.m_nNumDroppedFrames;
			g_PerfLastReprojected = stats.m_nNumReprojectedFrames;
			g_PerfHasCompositorCounters = true;
		}

		MEMORYSTATUSEX memory = {};
		memory.dwLength = sizeof(memory);
		GlobalMemoryStatusEx(&memory);

		SYSTEMTIME time;
		GetLocalTime(&time);
		char start[128];
		snprintf(start, sizeof(start), "%02d:%02d:%02d,%.1f,%u,%u,%llu", time.wHour, time.wMinute, time.wSecond, g_PerfFramesInWindow / elapsed,
			dropped, reprojected, (memory.ullTotalVirtual - memory.ullAvailVirtual) / (1024 * 1024));
		row = start;

		{
			std::lock_guard<std::mutex> lock(g_PerfMutex);
			for (std::vector<float> &samples : g_PerfSamples)
			{
				AppendSectionStats(row, samples);
				samples.clear();
			}
		}

		Enqueue(true, row);
		g_PerfFramesInWindow = 0;
	}
}
