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
		Event_GrabbedFresh = 1 << 4,
		Event_Inserted = 1 << 5,
		Event_ShellInserted = 1 << 6,
		Event_GrabbedBolt = 1 << 7,
		Event_BoltPulled = 1 << 8,   // pulled all the way back (or worked with the button)
		Event_BoltReleased = 1 << 9, // let go after that: it goes forward
		// Pulled with a live round in the chamber: it comes out unfired, so the weapon has one less. The weapon's clip
		// shows it once the server takes it.
		Event_RoundEjected = 1 << 10,
		// A pump or bolt action worked after a shot: the empty case comes out
		Event_CaseEjected = 1 << 11,
	};

	struct Inputs
	{
		HandMath::Vec3 palm;
		HandMath::Vec3 held;        // center of the magazine or shell in the hand: it goes in when this reaches the socket
		HandMath::Vec3 socket;      // where the magazine goes, or where shells are loaded
		HandMath::Vec3 magazineBase; // the end of the magazine that sticks out: it can be taken anywhere from there to the socket
		HandMath::Vec3 boltGrab;    // what the hand takes to pull the bolt
		HandMath::Vec3 boltAxis;    // the bolt is pulled against this direction
		HandMath::Vec3 weaponPoint; // moves with the weapon: the bolt's pull is how the hand moved relative to it, so
		                            // walking doesn't add to it or take from it
		HandMath::Vec3 waist;
		float unitsPerMeter = 43.2f;
		float boltTravel = 2.5f;    // game units
		bool grip = false;          // takes the magazine and the shells
		bool trigger = false;       // the left trigger: takes the bolt, so the grip can keep holding the weapon
		bool holdsPump = false;     // the grip holds the weapon by its pump: the trigger works it from wherever the hand is
		bool pumpEveryShot = false; // pump action: the pump has to be worked after every shot
		int clip = 0;
		int capacity = 0;
		int reserve = 0;
		bool shells = false;        // loaded one by one
		bool hasBolt = false;
		bool handFree = true;       // the hand holds nothing of its own
		bool onlyFreeHand = true;   // only a free hand reloads: takes the magazine out, a new one from the bag, the bolt
		                            // (ReloadNeedsFreeHand). Otherwise a busy hand does too, but takes the bolt only when
		                            // the weapon needs it: its trigger is its own weapon's. A pump held with both hands is
		                            // worked either way.
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
	// How many updates a round thrown out waits for the weapon's clip to show it (about a second): a host whose mod
	// doesn't take it never does
	const int kEjectWaitUpdates = 90;

	inline float DistanceToSegment(const HandMath::Vec3 &point, const HandMath::Vec3 &a, const HandMath::Vec3 &b)
	{
		using namespace HandMath;
		const Vec3 along = b - a;
		const float lengthSquared = Dot(along, along);
		const float t = lengthSquared > 1e-6f ? std::clamp(Dot(point - a, along) / lengthSquared, 0.0f, 1.0f) : 0.0f;
		return Length(point - (a + along * t));
	}

	class Machine
	{
	public:
		State GetState() const { return m_State; }
		bool NeedsCycle() const { return m_NeedsCycle; }
		bool MagazineIsOut() const { return m_MagazineOut; }

		// From taking the magazine out until the weapon is ready to fire again
		bool ReloadInProgress() const { return m_ReloadInProgress; }

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
			m_ReloadInProgress = false;
			m_BoltPull = 0.0f;
			m_BoltPulledThisGrab = false;
			m_GripWasDown = false;
			m_TriggerWasDown = false;
			m_PressUsed = false;
			m_LastClip = -1;
			m_EjectsPending = 0;
			m_EjectWait = 0;
			m_SpentCase = false;
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
			const float toMagazine = DistanceToSegment(in.palm, in.socket, in.magazineBase);
			const float heldToSocket = Length(in.held - in.socket);
			const float toWaist = Length(in.palm - in.waist);
			const float toBolt = Length(in.palm - in.boltGrab);
			const float meters = in.unitsPerMeter;
			const bool canReload = in.capacity > 0 && in.clip < in.capacity && in.reserve > 0;
			// The waist always gives one while there is ammo left, even with the weapon full
			const bool canTake = in.capacity > 0 && in.reserve > 0;
			// What the hand may start (Inputs::onlyFreeHand)
			const bool mayReload = in.handFree || !in.onlyFreeHand;

			// Rounds the bolt threw out (Event_RoundEjected) leave the clip when the server takes them: that isn't a shot
			int dropped = m_LastClip > 0 && in.clip < m_LastClip ? m_LastClip - in.clip : 0;
			const int ejectedNow = std::min(dropped, m_EjectsPending);
			m_EjectsPending -= ejectedNow;
			dropped -= ejectedNow;
			if (m_EjectsPending > 0 && --m_EjectWait <= 0)
				m_EjectsPending = 0;

			// The weapon ran out: the bolt stays back until it's pulled
			if (in.clip == 0 && m_LastClip != 0 && in.hasBolt && in.capacity > 0)
				m_NeedsCycle = true;
			// A pump action fired: the empty shell stays in until the pump is worked
			if (in.pumpEveryShot && in.hasBolt && dropped > 0)
			{
				m_NeedsCycle = true;
				m_SpentCase = true;
			}
			m_LastClip = in.clip;
			const bool mayTakeBolt = in.handFree || (mayReload && m_NeedsCycle);

			unsigned events = Event_None;

			switch (m_State)
			{
			case State::Idle:
				if (in.hasBolt && ((in.holdsPump && in.trigger) || (mayTakeBolt && triggerPressed && toBolt < kBoltGrabRadius * meters)))
				{
					// Any time, not only when it's needed: it's seen moving either way. The hand holding the pump takes it
					// as soon as the trigger is held, without pressing it again.
					m_State = State::HoldingBolt;
					m_BoltGrabOffset = in.palm - in.weaponPoint;
					m_BoltPull = 0.0f;
					m_BoltPulledThisGrab = false;
					events |= Event_GrabbedBolt;
				}
				else if (pressed && mayReload && !in.shells && canReload && toMagazine < kGrabRadius * meters)
				{
					m_State = State::HoldingOld;
					m_MagazineOut = false;
					m_PressUsed = true;
					m_OldGrabOffset = in.palm - in.socket;
					events |= Event_GrabbedOld;
				}
				else if (pressed && mayReload && canTake && toWaist < kPouchRadius * meters)
				{
					// One can be taken at any time. It only goes in once the old one is out.
					m_State = State::HoldingFresh;
					m_PressUsed = true;
					events |= Event_GrabbedFresh;
				}
				break;

			case State::HoldingOld:
				// How far it moved from where it was taken, with the weapon moving too
				if (!m_MagazineOut && Length((in.palm - in.socket) - m_OldGrabOffset) > kPullDistance * meters)
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
				else if (pressed && mayReload && toWaist < kPouchRadius * meters)
				{
					m_State = State::HoldingFresh;
					m_PressUsed = true;
					events |= Event_GrabbedFresh;
				}
				break;

			case State::HoldingFresh:
				// A shell only goes in while there is room for it, a magazine once the old one is out
				if (heldToSocket < kInsertRadius * meters && (in.shells ? in.clip < in.capacity : m_MagazineOut))
				{
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
				// Relative to the weapon, which moves with the player
				float pulled = Dot((in.palm - in.weaponPoint) - m_BoltGrabOffset, in.boltAxis) * -1.0f;
				if (pulled < 0.0f)
				{
					// The hand went further forward than where it took it: the pull counts from there
					m_BoltGrabOffset = m_BoltGrabOffset - in.boltAxis * pulled;
					pulled = 0.0f;
				}
				m_BoltPull = std::min(pulled, in.boltTravel);

				if (!m_BoltPulledThisGrab && m_BoltPull >= in.boltTravel * kBoltPullNeeded)
				{
					m_BoltPulledThisGrab = true;
					// What the weapon has, less what was thrown out that its clip doesn't show yet
					int rounds = in.clip - m_EjectsPending;
					if (m_SpentCase)
					{
						m_SpentCase = false;
						events |= Event_CaseEjected;
					}
					else if (!m_NeedsCycle && rounds > 0)
					{
						// A live round was in the chamber: it comes out as if it had been fired, and the next one goes in
						--rounds;
						++m_EjectsPending;
						m_EjectWait = kEjectWaitUpdates;
						events |= Event_RoundEjected;
					}
					// Chambers a round, if there's one to chamber. The last one thrown out leaves it empty, held back.
					m_NeedsCycle = rounds <= 0;
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

			if (m_MagazineOut)
				m_ReloadInProgress = true;
			else if (!m_NeedsCycle)
				m_ReloadInProgress = false;

			return events;
		}

	private:
		State m_State = State::Idle;
		bool m_MagazineOut = false;
		bool m_NeedsCycle = false;
		bool m_PumpEveryShot = false;
		bool m_ReloadInProgress = false;
		HandMath::Vec3 m_OldGrabOffset;
		float m_BoltPull = 0.0f;
		float m_BoltTravel = 0.0f;
		bool m_BoltPulledThisGrab = false;
		HandMath::Vec3 m_BoltGrabOffset; // from the weapon point to the hand, where the pull counts from
		bool m_GripWasDown = false;
		bool m_TriggerWasDown = false;
		bool m_PressUsed = false;
		int m_LastClip = -1;
		int m_EjectsPending = 0;  // rounds thrown out that the clip doesn't show yet
		int m_EjectWait = 0;      // updates left to wait for them
		bool m_SpentCase = false; // a pump or bolt action fired: its empty case is in the chamber
	};
}
