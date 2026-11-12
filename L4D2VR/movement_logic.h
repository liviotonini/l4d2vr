#pragma once
#include <algorithm>
#include <cmath>

// Moving the player: with the body (roomscale, a step taken for real) and with the thumbstick, how far it's pushed.
// No game or SteamVR types, so it can be tested on its own (tests/). Distances are game units, on the ground (x, y).
namespace Movement
{
	struct Vec2
	{
		float x = 0.0f, y = 0.0f;

		Vec2() {}
		Vec2(float x, float y) : x(x), y(y) {}
		Vec2 operator+(const Vec2 &v) const { return Vec2(x + v.x, y + v.y); }
		Vec2 operator-(const Vec2 &v) const { return Vec2(x - v.x, y - v.y); }
		Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
	};

	inline float Dot(const Vec2 &a, const Vec2 &b) { return a.x * b.x + a.y * b.y; }
	inline float Length(const Vec2 &v) { return sqrtf(Dot(v, v)); }

	// How far the camera goes with the body this frame. The part of the body's move that takes it to where the head was
	// is the body catching up with a step taken for real: the head is already there, so the camera stays. The rest (the
	// thumbstick, being pushed or carried, falling, a teleport) takes the camera along. The body can also go up to
	// `overshoot` past the head without taking it: it doesn't stop dead after a step. `headFromBody` is from the body
	// before the move to the head.
	inline Vec2 CameraFollow(const Vec2 &bodyMove, const Vec2 &headFromBody, float overshoot = 0.0f)
	{
		const float distance = Length(headFromBody);
		const float moved = Length(bodyMove);
		if (moved < 1e-6f)
			return Vec2();
		const Vec2 toward = distance > 1e-4f ? headFromBody * (1.0f / distance) : bodyMove * (1.0f / moved);
		const float caughtUp = std::clamp(Dot(bodyMove, toward), 0.0f, distance + overshoot);
		return bodyMove - toward * caughtUp;
	}

	// The thumbstick as a share of the top speed: nothing in its dead zone, then up to 1 pushed all the way, in the
	// direction it's pushed (x right, y forward)
	inline Vec2 StickShare(float x, float y, float deadZone)
	{
		const float length = sqrtf(x * x + y * y);
		if (length <= deadZone || length < 1e-4f)
			return Vec2();
		const float share = std::min((length - deadZone) / (1.0f - deadZone), 1.0f);
		return Vec2(x, y) * (share / length);
	}

	// The speeds asked of the game for the thumbstick's share (StickShare): from `minimumSpeed`, the slowest the game's
	// movement keeps up (below it the ground's friction holds the player still), to `topSpeed` all the way
	inline Vec2 StickSpeeds(const Vec2 &share, float minimumSpeed, float topSpeed)
	{
		const float length = Length(share);
		if (length < 1e-4f)
			return Vec2();
		const float lowest = std::min(minimumSpeed, topSpeed);
		return share * ((lowest + length * (topSpeed - lowest)) / length);
	}

	// A velocity on the ground as the forward and side speeds of a usercmd, for the view's yaw (degrees): what the game's
	// movement makes of them is that velocity
	inline void ToCommand(const Vec2 &velocity, float yawDegrees, float &forward, float &side)
	{
		const float radians = yawDegrees * 3.14159265f / 180.0f;
		const Vec2 forwardAxis(cosf(radians), sinf(radians)), rightAxis(sinf(radians), -cosf(radians));
		forward = Dot(velocity, forwardAxis);
		side = Dot(velocity, rightAxis);
	}

	// No faster than `maxSpeed`, in whatever direction
	inline void LimitSpeed(float &forward, float &side, float maxSpeed)
	{
		const float speed = sqrtf(forward * forward + side * side);
		if (speed > maxSpeed && speed > 0.0f)
		{
			forward *= maxSpeed / speed;
			side *= maxSpeed / speed;
		}
	}

	struct RoomscaleSettings
	{
		float startDistance = 5.2f;   // the head that far from the body: the body starts walking to it (12 cm)
		float stopDistance = 1.3f;    // and stops this close (3 cm)
		float gain = 8.0f;            // speed per unit of distance, 1/s
		float minimumSpeed = 0.0f;    // asked at least, or the ground's friction holds the body still
		float startGrace = 0.4f;      // seconds after starting, while the body gets going, before it's judged
		float blockedTime = 0.4f;     // seconds without moving towards the head while asked to: blocked (a wall)
		float blockedShare = 0.25f;   // moving less than this share of what was asked counts as not moving
		float minimumAsked = 2.0f;    // units asked in that time, below which it isn't judged
		float resumeDistance = 2.2f;  // blocked, the head moving this much from where it was walks again (5 cm)
	};

	// The body walking to where the head is: started past a distance and stopped closer than another one, so leaning a
	// little and the head's sway don't move it. Stops when it doesn't get there (a wall, a table, held by an infected)
	// until the head moves.
	class Roomscale
	{
	public:
		// Every usercmd. `headFromBody`: from the body to the head now; `bodyMove`: how far the body moved since the last
		// call; `canWalk`: on the ground. Returns the velocity wanted for the body (units/s), towards the head.
		Vec2 Update(const Vec2 &headFromBody, const Vec2 &bodyMove, bool canWalk, float maxSpeed, float seconds, const RoomscaleSettings &s)
		{
			const float distance = Length(headFromBody);
			if (!canWalk)
			{
				Stop();
				m_Blocked = false;
				return Vec2();
			}

			if (m_Blocked)
			{
				if (Length(headFromBody - m_BlockedAt) < s.resumeDistance && distance >= s.stopDistance)
					return Vec2();
				m_Blocked = false;
			}

			// Past the head (the body still moving after it got there): arrived, it doesn't walk back
			if (m_Following && Dot(headFromBody, m_Direction) <= 0.0f)
				Stop();

			// Judged over a while, once it got going: how far it went towards the head, against how far it was asked to
			if (m_Following && m_Grace > 0.0f)
			{
				m_Grace -= seconds;
			}
			else if (m_Following && distance > 1e-4f)
			{
				m_Asked += m_LastSpeed * seconds;
				m_Moved += std::max(Dot(bodyMove, headFromBody * (1.0f / distance)), 0.0f);
				m_Watched += seconds;
				if (m_Watched >= s.blockedTime)
				{
					if (m_Asked >= s.minimumAsked && m_Moved < m_Asked * s.blockedShare)
					{
						Stop();
						m_Blocked = true;
						m_BlockedAt = headFromBody;
						return Vec2();
					}
					m_Asked = m_Moved = m_Watched = 0.0f;
				}
			}

			if (!m_Following && distance > s.startDistance)
			{
				m_Following = true;
				m_Grace = s.startGrace;
				m_Asked = m_Moved = m_Watched = 0.0f;
			}
			else if (m_Following && distance < s.stopDistance)
			{
				Stop();
			}
			if (!m_Following)
				return Vec2();

			m_LastSpeed = std::min(std::max(s.gain * distance, s.minimumSpeed), maxSpeed);
			m_Direction = headFromBody * (1.0f / distance);
			return m_Direction * m_LastSpeed;
		}

		bool Following() const { return m_Following; }
		bool Blocked() const { return m_Blocked; }

	private:
		void Stop()
		{
			m_Following = false;
			m_LastSpeed = 0.0f;
			m_Asked = m_Moved = m_Watched = 0.0f;
		}

		bool m_Following = false;
		bool m_Blocked = false;
		Vec2 m_BlockedAt;
		Vec2 m_Direction;             // the way it's walking
		float m_Grace = 0.0f;
		float m_LastSpeed = 0.0f;
		float m_Asked = 0.0f, m_Moved = 0.0f, m_Watched = 0.0f;
	};
}
