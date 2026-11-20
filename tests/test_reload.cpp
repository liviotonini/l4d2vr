#include "../L4D2VR/reload_logic.h"
#include "test_common.h"

using namespace Reload;
using HandMath::Vec3;

namespace
{
	const Vec3 kSocket(10.0f, 0.0f, 40.0f);
	const Vec3 kBolt(5.0f, 0.0f, 44.0f);
	const Vec3 kWaist(0.0f, 8.0f, 20.0f);
	const Vec3 kNowhere(0.0f, -40.0f, 60.0f);

	struct Rig
	{
		Machine machine;
		Inputs in;
		unsigned events = 0;

		Rig(int clip, int capacity, int reserve, bool shells, bool bolt = true)
		{
			in.socket = kSocket;
			in.magazineBase = kSocket;
			in.boltGrab = kBolt;
			in.boltAxis = Vec3(1.0f, 0.0f, 0.0f);
			in.waist = kWaist;
			in.palm = kNowhere;
			in.held = kNowhere;
			in.clip = clip;
			in.capacity = capacity;
			in.reserve = reserve;
			in.shells = shells;
			in.hasBolt = bolt;
			step();
		}

		void step() { events = machine.Update(in); }
		void pullMagazineOut()
		{
			moveTo(kSocket);
			grip(true);
			moveTo(kSocket + Vec3(0.0f, 0.0f, -8.0f));
			grip(false);
		}
		// What the hand holds is in the palm, unless a test puts it elsewhere
		void moveTo(const Vec3 &palm) { in.palm = palm; in.held = palm; step(); }
		void grip(bool down) { in.grip = down; step(); }
		void trigger(bool down) { in.trigger = down; step(); }
		// The trigger takes the bolt where it is and pulls it all the way back; `events` are the pull's
		void pullBolt() { moveTo(kBolt); trigger(true); moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f)); }
		// Forward again, and let go
		void releaseBolt() { moveTo(kBolt); trigger(false); }
		// A new magazine (or shell) from the waist, in: the events of when it went in
		unsigned insertFresh()
		{
			moveTo(kWaist);
			grip(true);
			moveTo(kSocket);
			const unsigned inserted = events;
			grip(false);
			return inserted;
		}
	};

	void TestMagazine()
	{
		printf("manual reload: magazine\n");
		Rig r(12, 50, 200, false);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.BlocksFire(12, false));

		// Pressing away from the weapon does nothing
		r.grip(true);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.HandIsBusy()); // free to take items from the body
		r.grip(false);

		// Grab the magazine
		r.moveTo(kSocket);
		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingOld);
		CHECK(r.events & Event_GrabbedOld);
		CHECK(r.machine.HandIsBusy());

		// Letting go without pulling it out leaves it in
		r.grip(false);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.MagazineIsOut());
		CHECK(!r.machine.BlocksFire(12, false));

		// Grab and pull it out
		r.grip(true);
		r.moveTo(kSocket + Vec3(0.0f, 0.0f, -8.0f));
		CHECK(r.events & Event_MagazineOut);
		CHECK(r.machine.MagazineIsOut());
		CHECK(r.machine.BlocksFire(12, false));

		// Let it fall
		r.grip(false);
		CHECK(r.events & Event_DroppedOld);
		CHECK(r.machine.GetState() == State::NeedFresh);
		CHECK(r.machine.BlocksFire(12, false));

		// Pressing somewhere else doesn't take one
		r.moveTo(kNowhere);
		r.grip(true);
		CHECK(r.machine.GetState() == State::NeedFresh);
		r.grip(false);

		// Take a new one from the waist
		r.moveTo(kWaist);
		r.grip(true);
		CHECK(r.events & Event_GrabbedFresh);
		CHECK(r.machine.GetState() == State::HoldingFresh);

		// Letting go puts it back
		r.grip(false);
		CHECK(r.machine.GetState() == State::NeedFresh);

		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingFresh);

		// Into the weapon
		r.moveTo(kSocket + Vec3(1.0f, 0.0f, -1.0f));
		CHECK(r.events & Event_Inserted);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.MagazineIsOut());
		CHECK(!r.machine.NeedsCycle()); // it had a round in the chamber
		CHECK(!r.machine.BlocksFire(50, false));
		r.grip(false);
	}

	void TestEmptyAndBolt()
	{
		printf("manual reload: empty weapon and bolt\n");
		Rig r(3, 50, 200, false);

		// Fires the last round
		r.in.clip = 0;
		r.step();
		CHECK(r.machine.NeedsCycle());
		CHECK(r.machine.BlocksFire(0, false)); // doesn't start the game's automatic reload

		// Takes the magazine out
		r.pullMagazineOut();
		CHECK(r.machine.GetState() == State::NeedFresh);
		CHECK(r.machine.ReloadInProgress());

		r.moveTo(kWaist);
		r.grip(true);
		r.moveTo(kSocket);
		CHECK(r.events & Event_Inserted);
		r.grip(false);

		// The server gave it ammo, but the bolt is still back
		r.in.clip = 50;
		r.in.reserve = 150;
		r.step();
		CHECK(r.machine.NeedsCycle());
		CHECK(r.machine.ReloadInProgress()); // until it's pulled
		CHECK(r.machine.BlocksFire(50, false));

		// The grip doesn't take the bolt: it's the trigger
		r.moveTo(kBolt);
		r.grip(true);
		CHECK(r.machine.GetState() != State::HoldingBolt);
		r.grip(false);

		// Until then it's drawn locked back
		CHECK(r.machine.BoltLockedBack());
		CHECK_NEAR(r.machine.BoltPull(), r.in.boltTravel, 0.001f);

		// Grab it and pull only a little: it stays back, still locked
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		CHECK(r.events & Event_GrabbedBolt);
		CHECK(!r.machine.HandIsBusy()); // the grip can keep holding the weapon
		r.moveTo(kBolt + Vec3(-0.5f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), r.in.boltTravel, 0.001f);
		CHECK(r.machine.NeedsCycle());
		CHECK(!(r.events & Event_BoltPulled));

		// Moving it forward doesn't count: the pull counts from the furthest forward the hand went. And it can't go past
		// its travel.
		r.moveTo(kBolt + Vec3(3.0f, 0.0f, 0.0f));
		CHECK(r.machine.NeedsCycle());
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), r.in.boltTravel, 0.001f);
		CHECK(r.events & Event_BoltPulled);
		CHECK(!(r.events & Event_RoundEjected)); // the chamber was empty: nothing comes out
		CHECK(!(r.events & Event_CaseEjected));
		CHECK(!r.machine.NeedsCycle());
		CHECK(!r.machine.ReloadInProgress());
		CHECK(!r.machine.BoltLockedBack());
		CHECK(r.machine.BlocksFire(50, false)); // still held back

		// Pressing the grip meanwhile doesn't let go of it
		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.grip(false);
		CHECK(r.machine.GetState() == State::HoldingBolt);

		// Going forward with the hand, it follows, and it's ready to be pulled again
		r.moveTo(kBolt + Vec3(3.0f - 0.5f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), 0.5f, 0.001f);
		CHECK(r.events & Event_BoltReleased);
		CHECK(r.machine.BlocksFire(50, false)); // more than a tenth of the way back
		r.moveTo(kBolt + Vec3(3.0f - 0.1f, 0.0f, 0.0f));
		CHECK(!r.machine.BlocksFire(50, false));

		r.trigger(false);
		CHECK(!(r.events & Event_BoltReleased)); // it already went forward
		CHECK(r.machine.GetState() == State::Idle);
		CHECK_NEAR(r.machine.BoltPull(), 0.0f, 0.001f);

		// It doesn't ask again until the weapon runs out again
		r.step();
		CHECK(!r.machine.NeedsCycle());

		// Pulled when it isn't needed, it throws out the round that was in the chamber and chambers the next
		CHECK(!r.machine.NeedsCycle());
		r.moveTo(kBolt);
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.moveTo(kBolt + Vec3(-1.0f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), 1.0f, 0.001f);
		CHECK(!(r.events & Event_RoundEjected));
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK(r.events & Event_BoltPulled);
		CHECK(r.events & Event_RoundEjected);
		r.trigger(false);
		CHECK(r.events & Event_BoltReleased);
		CHECK(!r.machine.NeedsCycle());
		r.in.clip = 49; // the server took it
		r.step();
		CHECK(!r.machine.NeedsCycle());

		// An empty weapon racked with nothing to chamber still needs it afterwards
		r.in.clip = 0;
		r.step();
		CHECK(r.machine.NeedsCycle());
		r.moveTo(kBolt);
		r.trigger(true);
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK(r.events & Event_BoltPulled);
		CHECK(r.machine.NeedsCycle());
		r.trigger(false);
	}

	// The player walks while pulling: the weapon and the hand both move with the player, a few units every frame
	void TestBoltWhileWalking()
	{
		printf("manual reload: bolt pulled while walking\n");
		const float step = 2.4f; // 220 units/s at 90 frames per second

		for (int direction = -1; direction <= 1; direction += 2)
		{
			Rig r(30, 30, 100, false);
			r.in.clip = 0;
			r.step();
			r.in.clip = 30;
			r.step();
			CHECK(r.machine.NeedsCycle());

			// Taken where it is
			Vec3 walked(0.0f, 0.0f, 0.0f);
			r.moveTo(kBolt);
			r.trigger(true);
			CHECK(r.machine.GetState() == State::HoldingBolt);

			// Walking with the hand still on the weapon pulls nothing, forward or back
			for (int frame = 0; frame < 10; ++frame)
			{
				walked = walked + r.in.boltAxis * (step * direction);
				r.in.weaponPoint = walked;
				r.moveTo(kBolt + walked);
				CHECK(!(r.events & Event_BoltPulled));
			}
			CHECK_NEAR(r.machine.BoltPull(), r.in.boltTravel, 0.001f); // drawn locked back, as before
			CHECK(r.machine.NeedsCycle());

			// Pulled back along the weapon a little every frame while walking: it's pulled
			bool pulled = false;
			for (int frame = 1; frame <= 6 && !pulled; ++frame)
			{
				walked = walked + r.in.boltAxis * (step * direction);
				r.in.weaponPoint = walked;
				r.moveTo(kBolt + walked - r.in.boltAxis * (0.6f * frame));
				pulled = (r.events & Event_BoltPulled) != 0;
			}
			CHECK(pulled);
			CHECK(!r.machine.NeedsCycle());
			r.trigger(false);
		}
	}

	void TestMagazineFromWaist()
	{
		printf("manual reload: new magazine straight from the waist\n");
		Rig r(7, 15, 999, false);

		// Taken without taking the old one out first
		r.moveTo(kWaist);
		r.grip(true);
		CHECK(r.events & Event_GrabbedFresh);
		CHECK(r.machine.GetState() == State::HoldingFresh);
		CHECK(r.machine.HandIsBusy());
		CHECK(!r.machine.MagazineIsOut());
		CHECK(!r.machine.BlocksFire(7, false)); // the old one is still in

		// Let go: back to the waist, nothing changed
		r.grip(false);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.BlocksFire(7, false));

		// It doesn't go in while the old one is in the weapon
		r.grip(true);
		r.moveTo(kSocket + Vec3(0.0f, 0.0f, -3.0f));
		CHECK(!(r.events & Event_Inserted));
		CHECK(r.machine.GetState() == State::HoldingFresh);
		r.grip(false);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.ReloadInProgress());

		// The old one out, a new one goes in
		r.pullMagazineOut();
		CHECK(r.machine.GetState() == State::NeedFresh);
		r.moveTo(kWaist);
		r.grip(true);
		r.moveTo(kSocket + Vec3(0.0f, 0.0f, -3.0f));
		CHECK(r.events & Event_Inserted);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.MagazineIsOut());
		CHECK(!r.machine.ReloadInProgress()); // it had a round in the chamber
		r.grip(false);

		// It goes in when the magazine reaches the weapon, wherever the palm is
		Rig s(7, 15, 999, false);
		s.pullMagazineOut();
		s.moveTo(kWaist);
		s.grip(true);
		s.in.palm = kSocket + Vec3(0.0f, 0.0f, -6.0f); // 14 cm away
		s.in.held = kSocket + Vec3(0.0f, 0.0f, -3.0f); // the magazine sticks out of the hand towards it
		s.step();
		CHECK(s.events & Event_Inserted);

		// Full, one can still be taken, and it stays in the hand
		Rig full(15, 15, 999, false);
		full.moveTo(kWaist);
		full.grip(true);
		CHECK(full.machine.GetState() == State::HoldingFresh);
		full.moveTo(kSocket);
		CHECK(!(full.events & Event_Inserted));
		full.grip(false);

		// A magazine can be taken by the end that sticks out, far from its center, and it isn't out until it's pulled
		Rig base(7, 15, 999, false);
		base.in.magazineBase = kSocket + Vec3(0.0f, 0.0f, -7.0f);
		base.moveTo(kSocket + Vec3(0.0f, 0.0f, -11.0f)); // 25 cm from the center, 9 cm from the end
		base.grip(true);
		CHECK(base.machine.GetState() == State::HoldingOld);
		CHECK(!base.machine.MagazineIsOut());
		base.step();
		CHECK(!base.machine.MagazineIsOut());
		base.moveTo(kSocket + Vec3(0.0f, 0.0f, -14.5f)); // 8 cm further
		CHECK(base.machine.MagazineIsOut());
		CHECK(base.events & Event_MagazineOut);
		base.grip(false);

		// No ammo left: nothing to take
		Rig none(10, 50, 0, false);
		none.moveTo(kWaist);
		none.grip(true);
		CHECK(none.machine.GetState() == State::Idle);
		CHECK(!none.machine.HandIsBusy()); // free to take an item from the body
	}

	void TestPumpEveryShot()
	{
		printf("manual reload: pump after every shot\n");
		Rig r(8, 8, 50, true);
		r.in.pumpEveryShot = true;
		r.step();
		CHECK(!r.machine.NeedsCycle());
		CHECK(!r.machine.BlocksFire(8, true));

		// A shot: the pump has to be worked before the next
		r.in.clip = 7;
		r.step();
		CHECK(r.machine.NeedsCycle());
		CHECK(r.machine.BlocksFire(7, true));
		CHECK(!r.machine.BoltLockedBack()); // the pump stays forward
		CHECK_NEAR(r.machine.BoltPull(), 0.0f, 0.001f);

		// Holding the shotgun with both hands, the trigger takes the pump from wherever the hand is
		r.in.holdsPump = true;
		r.grip(true);
		r.moveTo(kBolt + Vec3(4.0f, 0.0f, 3.0f)); // 12 cm from where the pump is measured
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		CHECK(!r.machine.HandIsBusy());

		// Back: it's drawn following the hand, and chambers the next shell at the end
		r.moveTo(kBolt + Vec3(4.0f - 1.5f, 0.0f, 3.0f));
		CHECK_NEAR(r.machine.BoltPull(), 1.5f, 0.001f);
		CHECK(r.machine.NeedsCycle());
		r.moveTo(kBolt + Vec3(4.0f - 6.0f, 0.0f, 3.0f));
		CHECK(r.events & Event_BoltPulled);
		CHECK(!r.machine.NeedsCycle());
		CHECK(r.machine.BlocksFire(7, true)); // not while it's back

		// And forward again
		r.moveTo(kBolt + Vec3(4.0f, 0.0f, 3.0f));
		CHECK(r.events & Event_BoltReleased);
		CHECK(!r.machine.BlocksFire(7, true));
		r.trigger(false);

		// Two more shots, each needing it
		for (int clip = 6; clip >= 5; --clip)
		{
			r.in.clip = clip;
			r.step();
			CHECK(r.machine.BlocksFire(clip, true));
			r.trigger(true);
			r.moveTo(kBolt + Vec3(4.0f - 6.0f, 0.0f, 3.0f));
			r.moveTo(kBolt + Vec3(4.0f, 0.0f, 3.0f));
			r.trigger(false);
			CHECK(!r.machine.BlocksFire(clip, true));
		}

		// Several in a row without letting go of the trigger
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		for (int clip = 4; clip >= 3; --clip)
		{
			r.in.clip = clip;
			r.step();
			CHECK(r.machine.BlocksFire(clip, true));
			r.moveTo(kBolt + Vec3(4.0f - 6.0f, 0.0f, 3.0f));
			CHECK(r.events & Event_BoltPulled);
			CHECK(!r.machine.NeedsCycle());
			r.moveTo(kBolt + Vec3(4.0f, 0.0f, 3.0f));
			CHECK(r.events & Event_BoltReleased);
			CHECK(!r.machine.BlocksFire(clip, true));
		}
		r.trigger(false);

		// Already holding the trigger when the hand takes the weapon: no need to press it again
		r.in.holdsPump = false;
		r.moveTo(kNowhere);
		r.trigger(true);
		CHECK(r.machine.GetState() == State::Idle); // not on the pump yet, and far from it
		r.in.holdsPump = true;
		r.step();
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.trigger(false);

		// Loading a shell isn't a shot
		r.moveTo(kWaist);
		r.trigger(false);
		r.grip(false);
		r.grip(true);
		r.moveTo(kSocket);
		CHECK(r.events & Event_ShellInserted);
		r.grip(false);
		r.in.clip = 6;
		r.step();
		CHECK(!r.machine.NeedsCycle());

		// Without both hands on it, the trigger only takes the pump near it
		r.in.holdsPump = false;
		r.in.clip = 5;
		r.step();
		r.moveTo(kNowhere);
		r.trigger(true);
		CHECK(r.machine.GetState() == State::Idle);
		r.trigger(false);
		r.moveTo(kBolt);
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.trigger(false);

		// Without the mod on the host there is no bolt to work: the game pumps by itself
		Rig g(8, 8, 50, true, false);
		g.in.pumpEveryShot = true;
		g.in.clip = 7;
		g.step();
		CHECK(!g.machine.NeedsCycle());
	}

	void TestShells()
	{
		printf("manual reload: shells\n");
		Rig r(6, 8, 50, true);

		// Grabbing near the weapon doesn't take the "magazine"
		r.moveTo(kSocket);
		r.grip(true);
		CHECK(r.machine.GetState() == State::Idle);
		r.grip(false);

		// Two shells, one by one
		for (int i = 0; i < 2; ++i)
		{
			r.moveTo(kWaist);
			r.grip(true);
			CHECK(r.events & Event_GrabbedFresh);
			CHECK(r.machine.GetState() == State::HoldingFresh);
			CHECK(!r.machine.BlocksFire(r.in.clip, true)); // can still shoot with the other hand

			r.moveTo(kSocket);
			CHECK(r.events & Event_ShellInserted);
			CHECK(r.machine.GetState() == State::Idle);
			r.grip(false);

			r.in.clip += 1;
			r.in.reserve -= 1;
			r.step();
		}

		// Full: a shell can be taken, but it doesn't go in
		CHECK(r.in.clip == 8);
		r.moveTo(kWaist);
		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingFresh);
		r.moveTo(kSocket);
		CHECK(!(r.events & Event_ShellInserted));
		CHECK(r.machine.GetState() == State::HoldingFresh);
		r.grip(false);
		CHECK(r.machine.GetState() == State::Idle);

		// Dropping a shell doesn't need anything else
		r.in.clip = 5;
		r.moveTo(kWaist);
		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingFresh);
		r.grip(false);
		CHECK(r.machine.GetState() == State::Idle);

		// From empty it needs the pump
		r.in.clip = 0;
		r.step();
		r.moveTo(kWaist);
		r.grip(true);
		r.moveTo(kSocket);
		CHECK(r.events & Event_ShellInserted);
		r.grip(false);
		r.in.clip = 1;
		r.step();
		CHECK(r.machine.BlocksFire(1, true));

		// Pumped holding the weapon with both hands: the grip is held on the pump and the trigger pulls it
		r.moveTo(kBolt);
		r.grip(true);
		CHECK(!r.machine.HandIsBusy());
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		CHECK(!r.machine.HandIsBusy());
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK(r.events & Event_BoltPulled);
		CHECK(r.machine.BlocksFire(1, true)); // until it goes forward again
		r.trigger(false);
		CHECK(!r.machine.BlocksFire(1, true));
		r.grip(false);
		CHECK(r.machine.GetState() == State::Idle);
	}

	void TestEdges()
	{
		printf("manual reload: edge cases\n");

		// No reserve: the magazine stays in, nothing to replace it with
		{
			Rig r(10, 50, 0, false);
			r.moveTo(kSocket);
			r.grip(true);
			CHECK(r.machine.GetState() == State::Idle);
			r.grip(false);
		}

		// Full magazine
		{
			Rig r(50, 50, 200, false);
			r.moveTo(kSocket);
			r.grip(true);
			CHECK(r.machine.GetState() == State::Idle);
		}

		// The reserve runs out while the magazine is out (another player took the ammo): back to normal, not stuck
		{
			Rig r(10, 50, 100, false);
			r.pullMagazineOut();
			CHECK(r.machine.GetState() == State::NeedFresh);
			r.in.reserve = 0;
			r.step();
			CHECK(r.machine.GetState() == State::Idle);
			CHECK(!r.machine.MagazineIsOut());
		}

		// A weapon without a bolt doesn't ask for it
		{
			Rig r(1, 15, 30, false, false);
			r.in.clip = 0;
			r.step();
			CHECK(!r.machine.NeedsCycle());
		}

		// Holding the grip while arriving doesn't grab: it has to be pressed there
		{
			Rig r(10, 50, 100, false);
			r.grip(true);
			r.moveTo(kSocket);
			CHECK(r.machine.GetState() == State::Idle);
			r.grip(false);
		}

		// Same with the trigger and the bolt
		{
			Rig r(1, 50, 100, false);
			r.in.clip = 0;
			r.step();
			r.in.clip = 50;
			r.step();
			CHECK(r.machine.NeedsCycle());
			r.trigger(true);
			r.moveTo(kBolt);
			CHECK(r.machine.GetState() == State::Idle);
			r.trigger(false);
			r.trigger(true);
			CHECK(r.machine.GetState() == State::HoldingBolt);
		}

		// The trigger doesn't take a magazine or a shell
		{
			Rig r(10, 50, 100, false);
			r.moveTo(kSocket);
			r.trigger(true);
			CHECK(r.machine.GetState() == State::Idle);
			r.trigger(false);

			Rig s(3, 8, 50, true);
			s.moveTo(kWaist);
			s.trigger(true);
			CHECK(s.machine.GetState() == State::Idle);
		}

		// Reset (weapon changed) clears everything
		{
			Rig r(0, 50, 100, false);
			r.pullMagazineOut();
			r.machine.Reset();
			CHECK(r.machine.GetState() == State::Idle);
			CHECK(!r.machine.MagazineIsOut());
			CHECK(!r.machine.NeedsCycle());
			CHECK(!r.machine.BlocksFire(30, false));
		}

		// The radius follows the world scale
		{
			Rig r(10, 50, 100, false);
			r.in.unitsPerMeter = 86.4f;
			r.moveTo(kSocket + Vec3(0.0f, 0.0f, 9.0f)); // 10 cm at double scale, out of the default reach in units
			r.grip(true);
			CHECK(r.machine.GetState() == State::HoldingOld);
			r.grip(false);
		}
	}

	// A hand holding something of its own (the other hand's weapon, an item), ReloadNeedsFreeHand on and off
	void TestBusyHand()
	{
		printf("manual reload: a hand holding something\n");

		// Only a free hand reloads: nothing is started, not the magazine, the bag or the bolt
		{
			Rig r(0, 30, 100, false);
			r.in.handFree = false;
			r.in.onlyFreeHand = true;
			r.step();
			CHECK(r.machine.NeedsCycle());
			r.moveTo(kSocket);
			r.grip(true);
			CHECK(r.machine.GetState() == State::Idle);
			CHECK(!r.machine.HandIsBusy()); // free to do what the grab does otherwise
			r.grip(false);
			r.moveTo(kWaist);
			r.grip(true);
			CHECK(r.machine.GetState() == State::Idle);
			r.grip(false);
			r.moveTo(kBolt);
			r.trigger(true);
			CHECK(r.machine.GetState() == State::Idle);
			r.trigger(false);

			// Empty again, it does
			r.in.handFree = true;
			r.moveTo(kSocket);
			r.grip(true);
			CHECK(r.machine.GetState() == State::HoldingOld);
			r.moveTo(kSocket + Vec3(0.0f, 0.0f, -8.0f));
			r.grip(false);
			CHECK(r.machine.GetState() == State::NeedFresh);

			// A magazine out stays out while the hand holds something
			r.in.handFree = false;
			r.moveTo(kWaist);
			r.grip(true);
			CHECK(r.machine.GetState() == State::NeedFresh);
			r.grip(false);
		}

		// A pump held with both hands is worked anyway
		{
			Rig r(8, 8, 50, true);
			r.in.pumpEveryShot = true;
			r.in.handFree = false;
			r.in.onlyFreeHand = true;
			r.in.clip = 7;
			r.step();
			CHECK(r.machine.NeedsCycle());
			r.in.holdsPump = true;
			r.trigger(true);
			CHECK(r.machine.GetState() == State::HoldingBolt);
			r.trigger(false);
		}

		// A busy hand reloads too: magazine out, a new one from the bag (shells too), in
		{
			Rig r(10, 30, 100, false);
			r.in.handFree = false;
			r.in.onlyFreeHand = false;
			r.pullMagazineOut();
			CHECK(r.machine.GetState() == State::NeedFresh);
			r.moveTo(kWaist);
			r.grip(true);
			CHECK(r.machine.GetState() == State::HoldingFresh);
			r.moveTo(kSocket);
			CHECK(r.events & Event_Inserted);
			r.grip(false);

			Rig s(3, 8, 50, true);
			s.in.handFree = false;
			s.in.onlyFreeHand = false;
			s.moveTo(kWaist);
			s.grip(true);
			CHECK(s.machine.GetState() == State::HoldingFresh);
			s.moveTo(kSocket);
			CHECK(s.events & Event_ShellInserted);
			s.grip(false);
		}

		// ...but its trigger takes the bolt only when the weapon needs it: it's its own weapon's trigger otherwise
		{
			Rig r(5, 30, 100, false);
			r.in.handFree = false;
			r.in.onlyFreeHand = false;
			r.moveTo(kBolt);
			r.trigger(true);
			CHECK(r.machine.GetState() == State::Idle);
			r.trigger(false);

			r.in.clip = 0;
			r.step();
			CHECK(r.machine.NeedsCycle());
			r.trigger(true);
			CHECK(r.machine.GetState() == State::HoldingBolt);
			r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
			CHECK(r.events & Event_BoltPulled);
			r.trigger(false);
			CHECK(r.machine.GetState() == State::Idle);

			// A free hand takes it any time
			r.in.handFree = true;
			r.moveTo(kBolt);
			r.trigger(true);
			CHECK(r.machine.GetState() == State::HoldingBolt);
			r.trigger(false);
		}
	}
}

namespace
{
	// The bolt pulled with a live round in the chamber throws it out: one less in the weapon
	void TestRoundEjected()
	{
		printf("manual reload: a live round thrown out by the bolt\n");

		// Reloaded empty: the bolt chambers a round, nothing comes out
		{
			Rig r(1, 30, 100, false);
			r.in.clip = 0;
			r.step();
			r.pullMagazineOut();
			CHECK(r.insertFresh() & Event_Inserted);
			r.in.clip = 30;
			r.in.reserve = 70;
			r.step();
			CHECK(r.machine.NeedsCycle());
			r.pullBolt();
			CHECK(r.events & Event_BoltPulled);
			CHECK(!(r.events & (Event_RoundEjected | Event_CaseEjected)));
			r.releaseBolt();
			CHECK(!r.machine.NeedsCycle());
			CHECK(!r.machine.BlocksFire(30, false));
		}

		// Reloaded two short: full, with the round in the chamber, ready to fire. Pulling the bolt then throws that round out.
		{
			Rig r(28, 30, 100, false);
			r.pullMagazineOut();
			CHECK(r.insertFresh() & Event_Inserted);
			r.in.clip = 30;
			r.in.reserve = 98;
			r.step();
			CHECK(!r.machine.NeedsCycle());
			CHECK(!r.machine.BlocksFire(30, false));

			r.pullBolt();
			CHECK(r.events & Event_BoltPulled);
			CHECK(r.events & Event_RoundEjected);
			CHECK(!(r.events & Event_CaseEjected));
			CHECK(!r.machine.NeedsCycle()); // the next one went in
			r.releaseBolt();
			CHECK(!(r.events & Event_RoundEjected)); // only once per pull
			r.in.clip = 29; // the server took it
			r.step();
			CHECK(!r.machine.NeedsCycle());
			CHECK(!r.machine.BlocksFire(29, false));

			// Pulled back and forth with the trigger held: one round each time
			r.pullBolt();
			CHECK(r.events & Event_RoundEjected);
			r.moveTo(kBolt);
			CHECK(r.events & Event_BoltReleased);
			r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
			CHECK(r.events & Event_RoundEjected);
			r.releaseBolt();
			r.in.clip = 27; // both at once
			r.step();
			CHECK(!r.machine.NeedsCycle());
		}

		// The last round thrown out: nothing to chamber, so it's held back at once, before the server's count
		{
			Rig r(1, 30, 100, false);
			r.pullBolt();
			CHECK(r.events & Event_RoundEjected);
			CHECK(r.machine.NeedsCycle());
			r.releaseBolt();
			CHECK(r.machine.BoltLockedBack());
			CHECK(r.machine.BlocksFire(1, false));
			r.in.clip = 0;
			r.step();
			CHECK(r.machine.NeedsCycle());
			// Nothing more comes out of it
			r.pullBolt();
			CHECK(r.events & Event_BoltPulled);
			CHECK(!(r.events & Event_RoundEjected));
			r.releaseBolt();
			CHECK(r.machine.NeedsCycle());
		}

		// A pump worked without a shot throws out a live shell; the clip dropping for it isn't a shot
		{
			Rig r(8, 8, 50, true);
			r.in.pumpEveryShot = true;
			r.step();
			r.pullBolt();
			CHECK(r.events & Event_RoundEjected);
			CHECK(!(r.events & Event_CaseEjected));
			r.releaseBolt();
			r.in.clip = 7;
			r.step();
			CHECK(!r.machine.NeedsCycle());
			CHECK(!r.machine.BlocksFire(7, true));

			// A real shot: the pump throws its empty case out, and nothing else
			r.in.clip = 6;
			r.step();
			CHECK(r.machine.NeedsCycle());
			r.pullBolt();
			CHECK(r.events & Event_CaseEjected);
			CHECK(!(r.events & Event_RoundEjected));
			r.releaseBolt();
			CHECK(!r.machine.NeedsCycle());
			CHECK(!r.machine.BlocksFire(6, true));
		}

		// The shot is seen before the server's count of the shell thrown out: one of the two drops is still a shot
		{
			Rig r(8, 8, 50, true);
			r.in.pumpEveryShot = true;
			r.step();
			r.pullBolt();
			r.releaseBolt();
			r.in.clip = 7;
			r.step();
			r.in.clip = 6;
			r.step();
			CHECK(r.machine.NeedsCycle());
		}

		// A host whose mod doesn't take it: the clip never drops, and after a while a shot counts again
		{
			Rig r(8, 8, 50, true);
			r.in.pumpEveryShot = true;
			r.step();
			r.pullBolt();
			CHECK(r.events & Event_RoundEjected);
			r.releaseBolt();
			for (int i = 0; i < kEjectWaitUpdates; ++i)
				r.step();
			r.in.clip = 7;
			r.step();
			CHECK(r.machine.NeedsCycle());
		}

		// The last shell fired: once loaded, the pump throws out its case. The last one thrown out live leaves nothing.
		{
			Rig r(1, 8, 50, true);
			r.in.pumpEveryShot = true;
			r.step();
			r.in.clip = 0;
			r.step();
			CHECK(r.machine.NeedsCycle());
			CHECK(r.insertFresh() & Event_ShellInserted);
			r.in.clip = 1;
			r.step();
			r.pullBolt();
			CHECK(r.events & Event_CaseEjected);
			CHECK(!(r.events & Event_RoundEjected));
			r.releaseBolt();
			CHECK(!r.machine.NeedsCycle());

			r.pullBolt();
			CHECK(r.events & Event_RoundEjected);
			CHECK(r.machine.NeedsCycle());
			r.releaseBolt();
			r.in.clip = 0;
			r.step();
			r.insertFresh();
			r.in.clip = 1;
			r.step();
			r.pullBolt();
			CHECK(!(r.events & (Event_RoundEjected | Event_CaseEjected)));
			r.releaseBolt();
			CHECK(!r.machine.NeedsCycle());
		}

		// A weapon change forgets it all
		{
			Rig r(8, 8, 50, true);
			r.in.pumpEveryShot = true;
			r.step();
			r.pullBolt();
			r.releaseBolt();
			r.machine.Reset();
			r.step();
			r.in.clip = 7;
			r.step();
			CHECK(r.machine.NeedsCycle());
		}
	}
}

void RunReloadTests()
{
	TestMagazine();
	TestEmptyAndBolt();
	TestRoundEjected();
	TestMagazineFromWaist();
	TestPumpEveryShot();
	TestBoltWhileWalking();
	TestShells();
	TestEdges();
	TestBusyHand();
}
