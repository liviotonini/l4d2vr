#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Plays back another player's poses in the rhythm they were sent in, however the network bunched them on the way.
//
// Each pose carries the sender's clock in milliseconds (16 bits: it wraps every 65 s). The receiver shows them a little
// behind: as far behind as the slowest of the recent ones took to arrive, plus the longest time between two of them, so
// the pose after the one shown has always arrived and the motion is interpolated evenly. When the network gets better
// or worse that delay changes slowly (the motion plays a little slower or faster than it was made) instead of jumping.
// No game types: tested in tests/test_pose.cpp.
namespace PosePlayout
{
	const double kRestartMs = 1000.0;       // nothing for this long, or an older clock after it: a new stream
	const double kMarginMs = 4.0;
	const double kMaxGapMs = 100.0;         // poses lost on the way add at most this to the delay
	const double kMaxExtraDelayMs = 300.0;  // above the fastest recent arrival, however late one was
	const double kSlowDown = 0.25;          // the delay grows by at most this per millisecond (playback at 75%)
	const double kSpeedUp = 0.05;           // and shrinks by at most this (playback at 105%)
	const double kSnapMs = 250.0;           // further from where it should be than this: straight there

	template <typename Frame, int Capacity = 24, int History = 32>
	class Buffer
	{
	public:
		void Reset()
		{
			m_Count = 0;
			m_HistoryCount = 0;
			m_HistoryNext = 0;
			m_HaveOffset = false;
		}

		bool Empty() const { return m_Count == 0; }
		double LastArrivalMs() const { return m_LastArrival; }

		// How far behind the newest pose the playback was the last time it was sampled, and when that was
		double DelayBehindNewestMs() const { return m_Count > 0 ? m_Times[m_Count - 1] - m_LastPlayback : 0.0; }
		double LastSampleMs() const { return m_HaveOffset ? m_LastSampleMs : -1e300; }

		// How long the slowest and the fastest recent poses took to arrive, apart (the network's jitter)
		double JitterMs() const
		{
			double low = 0.0, high = 0.0;
			TransitRange(low, high);
			return high - low;
		}

		// False if it isn't newer than the last one (it came out of order or twice)
		bool Add(uint16_t senderMs, double arrivalMs, const Frame &frame)
		{
			double sender = senderMs;
			if (m_Count > 0)
			{
				const int newer = (int16_t)(uint16_t)(senderMs - m_NewestSender16);
				if (arrivalMs - m_LastArrival > kRestartMs)
					Reset();
				else if (newer <= 0)
					return false;
				else
					sender = m_Times[m_Count - 1] + newer;
			}

			const double gap = m_Count > 0 ? sender - m_Times[m_Count - 1] : -1.0;

			if (m_Count == Capacity)
			{
				for (int i = 1; i < Capacity; ++i)
				{
					m_Times[i - 1] = m_Times[i];
					m_Frames[i - 1] = m_Frames[i];
				}
				--m_Count;
			}
			m_Times[m_Count] = sender;
			m_Frames[m_Count] = frame;
			++m_Count;

			m_NewestSender16 = senderMs;
			m_LastArrival = arrivalMs;

			m_Transit[m_HistoryNext] = arrivalMs - sender;
			m_Gap[m_HistoryNext] = gap;
			m_HistoryNext = (m_HistoryNext + 1) % History;
			m_HistoryCount = std::min(m_HistoryCount + 1, History);
			return true;
		}

		// The two poses to show now and how far to go from the first to the second. False with no poses.
		bool Sample(double nowMs, const Frame *&a, const Frame *&b, float &t)
		{
			if (m_Count == 0)
				return false;

			const double target = TargetOffset();
			if (!m_HaveOffset || std::fabs(target - m_Offset) > kSnapMs)
			{
				m_Offset = target;
			}
			else
			{
				const double elapsed = std::clamp(nowMs - m_LastSampleMs, 0.0, 100.0);
				if (target > m_Offset)
					m_Offset = std::min(target, m_Offset + elapsed * kSlowDown);
				else
					m_Offset = std::max(target, m_Offset - elapsed * kSpeedUp);
			}
			m_HaveOffset = true;
			m_LastSampleMs = nowMs;

			const double playback = nowMs - m_Offset;
			m_LastPlayback = playback;

			if (playback <= m_Times[0])
			{
				a = b = &m_Frames[0];
				t = 0.0f;
				return true;
			}

			for (int i = 0; i + 1 < m_Count; ++i)
			{
				if (playback < m_Times[i + 1])
				{
					a = &m_Frames[i];
					b = &m_Frames[i + 1];
					t = (float)std::clamp((playback - m_Times[i]) / (m_Times[i + 1] - m_Times[i]), 0.0, 1.0);
					return true;
				}
			}

			// Past the newest: it stays there until the next one comes
			a = b = &m_Frames[m_Count - 1];
			t = 0.0f;
			return true;
		}

	private:
		// The arrival times are the receiver's clock minus the sender's: only their differences mean anything
		void TransitRange(double &low, double &high) const
		{
			low = 1e300;
			high = -1e300;
			for (int i = 0; i < m_HistoryCount; ++i)
			{
				low = std::min(low, m_Transit[i]);
				high = std::max(high, m_Transit[i]);
			}
			if (m_HistoryCount == 0)
				low = high = 0.0;
		}

		double TargetOffset() const
		{
			double low = 0.0, high = 0.0;
			TransitRange(low, high);

			double gap = 0.0;
			for (int i = 0; i < m_HistoryCount; ++i)
				gap = std::max(gap, m_Gap[i]);

			return std::min(high, low + kMaxExtraDelayMs) + std::min(gap, kMaxGapMs) + kMarginMs;
		}

		double m_Times[Capacity] = {};   // the sender's clock, unwrapped, oldest first
		Frame m_Frames[Capacity] = {};
		int m_Count = 0;
		uint16_t m_NewestSender16 = 0;
		double m_LastArrival = 0.0;

		double m_Transit[History] = {};
		double m_Gap[History] = {};
		int m_HistoryCount = 0;
		int m_HistoryNext = 0;

		double m_Offset = 0.0;           // the receiver's clock minus the sender's, for the playback
		bool m_HaveOffset = false;
		double m_LastSampleMs = 0.0;
		double m_LastPlayback = 0.0;
	};
}
