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
		NeedFresh,     // no magazine in the weapon, a new one is at the waist
		HoldingFresh,  // the hand has a new magazine or a shell
		HoldingBolt,
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
		Event_BoltPulled = 1 << 8,
	};

	struct Inputs
	{
		HandMath::Vec3 palm;
		HandMath::Vec3 socket;      // where the magazine goes, or where shells are loaded
		HandMath::Vec3 boltGrab;    // what the hand takes to pull the bolt
		HandMath::Vec3 boltAxis;    // the bolt is pulled against this direction
		HandMath::Vec3 waist;
		float unitsPerMeter = 43.2f;
		float boltTravel = 2.5f;    // game units
		bool grip = false;          // takes the magazine and the shells
		bool trigger = false;       // the left trigger: takes the bolt, so the grip can keep holding the weapon
		bool ejectPressed = false;  // the reload button: lets the magazine fall
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
	const float kInsertRadius = 0.09f;
	const float kBoltGrabRadius = 0.10f;
	// How much of its travel the bolt has to be pulled
	const float kBoltPullNeeded = 0.8f;

	class Machine
	{
	public:
		State GetState() const { return m_State; }
		bool NeedsCycle() const { return m_NeedsCycle; }
		bool MagazineIsOut() const { return m_MagazineOut; }
		float BoltPull() const { return m_State == State::HoldingBolt ? m_BoltPull : 0.0f; }

		// The left grip is being used by the reload, so it isn't taking items or holding the weapon with both hands.
		// The bolt is pulled with the trigger, so the grip can keep holding the weapon (the pump of a shotgun).
		bool HandIsBusy() const { return m_State == State::HoldingOld || m_State == State::HoldingFresh || m_PressUsed; }

		bool BlocksFire(int clip, bool shells) const
		{
			if (clip == 0 || m_NeedsCycle)
				return true;

			if (shells)
				return false;

			return m_MagazineOut || m_State == State::NeedFresh || m_State == State::HoldingFresh;
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

			const float toSocket = Length(in.palm - in.socket);
			const float toWaist = Length(in.palm - in.waist);
			const float toBolt = Length(in.palm - in.boltGrab);
			const float meters = in.unitsPerMeter;
			const bool canReload = in.capacity > 0 && in.clip < in.capacity && in.reserve > 0;

			// The weapon ran out: the bolt stays back until it's pulled
			if (in.clip == 0 && m_LastClip != 0 && in.hasBolt && in.capacity > 0)
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
					events |= Event_BoltPulled;
				}
				else if (in.ejectPressed && !in.shells && canReload)
				{
					m_MagazineOut = true;
					m_State = State::NeedFresh;
					events |= Event_Ejected;
				}
				else if (triggerPressed && m_NeedsCycle && in.clip > 0 && in.hasBolt && toBolt < kBoltGrabRadius * meters)
				{
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
				else if (pressed && in.shells && canReload && toWaist < kPouchRadius * meters)
				{
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
				if (toSocket < kInsertRadius * meters)
				{
					m_State = State::Idle;
					m_MagazineOut = false;
					events |= in.shells ? Event_ShellInserted : Event_Inserted;
				}
				else if (!in.grip)
				{
					m_State = in.shells ? State::Idle : State::NeedFresh;
				}
				break;

			case State::HoldingBolt:
			{
				const float pulled = Dot(in.palm - m_BoltGrabPalm, in.boltAxis) * -1.0f;
				m_BoltPull = std::clamp(pulled, 0.0f, in.boltTravel);

				if (!m_BoltPulledThisGrab && m_BoltPull >= in.boltTravel * kBoltPullNeeded)
				{
					m_BoltPulledThisGrab = true;
					m_NeedsCycle = false;
					events |= Event_BoltPulled;
				}

				if (!in.trigger)
				{
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
		float m_BoltPull = 0.0f;
		bool m_BoltPulledThisGrab = false;
		HandMath::Vec3 m_BoltGrabPalm;
		bool m_GripWasDown = false;
		bool m_TriggerWasDown = false;
		bool m_PressUsed = false;
		int m_LastClip = -1;
	};
}
