#pragma once
#include <algorithm>
#include "hand_math.h"

// The steps of a manual reload. No game or SteamVR types: it only knows where the hand and the parts of the weapon are,
// so the whole sequence can be tested on its own.
namespace Reload
{
	enum class State
	{
		Idle,
		HoldingOld,    // the hand has the magazine that was in the weapon
		NeedFresh,     // no magazine in the weapon
		HoldingFresh,  // the hand has a new magazine or a shell, taken from the waist
		HoldingBolt,   // the trigger holds the bolt, the slide or the pump
	};

	enum Event : unsigned
	{
		Event_None = 0,
		Event_GrabbedOld = 1 << 0,
		Event_MagazineOut = 1 << 1,
		Event_DroppedOld = 1 << 2,
		Event_Ejected = 1 << 3,
		Event_GrabbedFresh = 1 << 4,
		Event_Inserted = 1 << 5,
		Event_ShellInserted = 1 << 6,
		Event_GrabbedBolt = 1 << 7,
		Event_BoltPulled = 1 << 8,   // pulled all the way back (or worked with the button)
		Event_BoltReleased = 1 << 9, // let go after that: it goes forward
	};

	struct Inputs
	{
		HandMath::Vec3 palm;
		HandMath::Vec3 held;        // center of the magazine or shell in the hand: it goes in when this reaches the socket
		HandMath::Vec3 socket;      // where the magazine goes, or where shells are loaded
		HandMath::Vec3 boltGrab;    // what the hand takes to pull the bolt
		HandMath::Vec3 boltAxis;    // the bolt is pulled against this direction
		HandMath::Vec3 waist;
		float unitsPerMeter = 43.2f;
		float boltTravel = 2.5f;    // game units
		bool grip = false;          // takes the magazine and the shells
		bool trigger = false;       // the left trigger: takes the bolt, so the grip can keep holding the weapon
		bool ejectPressed = false;  // the reload button: lets the magazine fall
		bool holdsPump = false;     // the grip holds the weapon by its pump: the trigger works it from wherever the hand is
		bool pumpEveryShot = false; // pump action: the pump has to be worked after every shot
		int clip = 0;
		int capacity = 0;
		int reserve = 0;
		bool shells = false;        // loaded one by one
		bool hasBolt = false;
	};

	// Distances, in meters
	const float kGrabRadius = 0.11f;
	const float kPullDistance = 0.07f;
	const float kPouchRadius = 0.15f;
	const float kInsertRadius = 0.10f;
	const float kBoltGrabRadius = 0.12f;
	// How much of its travel the bolt has to be pulled, and how far forward it has to go again for the next pull
	// while the trigger is still held (a pump worked several times in a row)
	const float kBoltPullNeeded = 0.8f;
	const float kBoltReturned = 0.2f;
	// Pulled back more than this, the weapon doesn't fire
	const float kBoltBlocksFire = 0.1f;

	class Machine
	{
	public:
		State GetState() const { return m_State; }
		bool NeedsCycle() const { return m_NeedsCycle; }
		bool MagazineIsOut() const { return m_MagazineOut; }

		// Locked back: an empty weapon with a bolt or a slide shows it open until it's worked. Pump actions don't.
		bool BoltLockedBack() const { return m_NeedsCycle && !m_PumpEveryShot; }

		// How far back the bolt is drawn, game units
		float BoltPull() const
		{
			const float locked = BoltLockedBack() ? m_BoltTravel : 0.0f;
			return m_State == State::HoldingBolt ? std::max(m_BoltPull, locked) : locked;
		}

		// The left grip is being used by the reload, so it isn't taking items or holding the weapon with both hands.
		// The bolt is pulled with the trigger, so the grip can keep holding the weapon (the pump of a shotgun).
		bool HandIsBusy() const { return m_State == State::HoldingOld || m_State == State::HoldingFresh || m_PressUsed; }

		bool BlocksFire(int clip, bool shells) const
		{
			if (clip == 0 || m_NeedsCycle)
				return true;

			if (m_State == State::HoldingBolt && m_BoltPull > m_BoltTravel * kBoltBlocksFire)
				return true;

			// A new magazine in the hand while the old one is still in: it can still shoot
			return !shells && m_MagazineOut;
		}

		void Reset()
		{
			m_State = State::Idle;
			m_MagazineOut = false;
			m_NeedsCycle = false;
			m_BoltPull = 0.0f;
			m_BoltPulledThisGrab = false;
			m_GripWasDown = false;
			m_TriggerWasDown = false;
			m_PressUsed = false;
			m_LastClip = -1;
		}

		unsigned Update(const Inputs &in)
		{
			using namespace HandMath;

			const bool pressed = in.grip && !m_GripWasDown;
			const bool released = !in.grip && m_GripWasDown;
			m_GripWasDown = in.grip;
			if (released)
				m_PressUsed = false;

			const bool triggerPressed = in.trigger && !m_TriggerWasDown;
			m_TriggerWasDown = in.trigger;

			m_BoltTravel = in.boltTravel;
			m_PumpEveryShot = in.pumpEveryShot;

			const float toSocket = Length(in.palm - in.socket);
			const float heldToSocket = Length(in.held - in.socket);
			const float toWaist = Length(in.palm - in.waist);
			const float toBolt = Length(in.palm - in.boltGrab);
			const float meters = in.unitsPerMeter;
			const bool canReload = in.capacity > 0 && in.clip < in.capacity && in.reserve > 0;
			// The waist always gives one while there is ammo left, even with the weapon full
			const bool canTake = in.capacity > 0 && in.reserve > 0;

			// The weapon ran out: the bolt stays back until it's pulled
			if (in.clip == 0 && m_LastClip != 0 && in.hasBolt && in.capacity > 0)
				m_NeedsCycle = true;
			// A pump action fired: the empty shell stays in until the pump is worked
			if (in.pumpEveryShot && in.hasBolt && m_LastClip > 0 && in.clip < m_LastClip)
				m_NeedsCycle = true;
			m_LastClip = in.clip;

			unsigned events = Event_None;

			switch (m_State)
			{
			case State::Idle:
				if (in.ejectPressed && m_NeedsCycle && in.clip > 0)
				{
					// The button works the bolt too, for when the hands are busy
					m_NeedsCycle = false;
					events |= Event_BoltPulled | Event_BoltReleased;
				}
				else if (in.ejectPressed && !in.shells && canReload)
				{
					m_MagazineOut = true;
					m_State = State::NeedFresh;
					events |= Event_Ejected;
				}
				else if (in.hasBolt && ((in.holdsPump && in.trigger) || (triggerPressed && toBolt < kBoltGrabRadius * meters)))
				{
					// Any time, not only when it's needed: it's seen moving either way. The hand holding the pump takes it
					// as soon as the trigger is held, without pressing it again.
					m_State = State::HoldingBolt;
					m_BoltGrabPalm = in.palm;
					m_BoltPull = 0.0f;
					m_BoltPulledThisGrab = false;
					events |= Event_GrabbedBolt;
				}
				else if (pressed && !in.shells && canReload && toSocket < kGrabRadius * meters)
				{
					m_State = State::HoldingOld;
					m_MagazineOut = false;
					m_PressUsed = true;
					events |= Event_GrabbedOld;
				}
				else if (pressed && canTake && toWaist < kPouchRadius * meters)
				{
					// A new one straight from the waist: the old magazine comes out when it goes in
					m_State = State::HoldingFresh;
					m_PressUsed = true;
					events |= Event_GrabbedFresh;
				}
				break;

			case State::HoldingOld:
				if (!m_MagazineOut && toSocket > kPullDistance * meters)
				{
					m_MagazineOut = true;
					events |= Event_MagazineOut;
				}

				if (!in.grip)
				{
					if (m_MagazineOut)
					{
						m_State = State::NeedFresh;
						events |= Event_DroppedOld;
					}
					else
					{
						m_State = State::Idle;
					}
				}
				break;

			case State::NeedFresh:
				if (in.reserve <= 0)
				{
					// Nothing left to put in
					m_State = State::Idle;
					m_MagazineOut = false;
				}
				else if (pressed && toWaist < kPouchRadius * meters)
				{
					m_State = State::HoldingFresh;
					m_PressUsed = true;
					events |= Event_GrabbedFresh;
				}
				break;

			case State::HoldingFresh:
				// A shell only goes in while there is room for it
				if (heldToSocket < kInsertRadius * meters && (!in.shells || in.clip < in.capacity))
				{
					if (!in.shells && !m_MagazineOut)
						events |= Event_MagazineOut; // the old one falls out as the new one goes in
					m_State = State::Idle;
					m_MagazineOut = false;
					events |= in.shells ? Event_ShellInserted : Event_Inserted;
				}
				else if (!in.grip)
				{
					// Back to the waist
					m_State = (!in.shells && m_MagazineOut) ? State::NeedFresh : State::Idle;
				}
				break;

			case State::HoldingBolt:
			{
				float pulled = Dot(in.palm - m_BoltGrabPalm, in.boltAxis) * -1.0f;
				if (pulled < 0.0f)
				{
					// The hand went further forward than where it took it: the pull counts from there
					m_BoltGrabPalm = m_BoltGrabPalm - in.boltAxis * pulled;
					pulled = 0.0f;
				}
				m_BoltPull = std::min(pulled, in.boltTravel);

				if (!m_BoltPulledThisGrab && m_BoltPull >= in.boltTravel * kBoltPullNeeded)
				{
					m_BoltPulledThisGrab = true;
					// Chambers a round, if there's one to chamber
					if (in.clip > 0)
						m_NeedsCycle = false;
					events |= Event_BoltPulled;
				}
				else if (m_BoltPulledThisGrab && in.trigger && m_BoltPull <= in.boltTravel * kBoltReturned)
				{
					// Forward again with the trigger still held: ready to be pulled again
					m_BoltPulledThisGrab = false;
					events |= Event_BoltReleased;
				}

				if (!in.trigger)
				{
					if (m_BoltPulledThisGrab)
						events |= Event_BoltReleased;
					m_State = State::Idle;
					m_BoltPull = 0.0f;
				}
				break;
			}
			}

			return events;
		}

	private:
		State m_State = State::Idle;
		bool m_MagazineOut = false;
		bool m_NeedsCycle = false;
		bool m_PumpEveryShot = false;
		float m_BoltPull = 0.0f;
		float m_BoltTravel = 0.0f;
		bool m_BoltPulledThisGrab = false;
		HandMath::Vec3 m_BoltGrabPalm;
		bool m_GripWasDown = false;
		bool m_TriggerWasDown = false;
		bool m_PressUsed = false;
		int m_LastClip = -1;
	};
}
