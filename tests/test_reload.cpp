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

		void step() { events = machine.Update(in); in.ejectPressed = false; }
		// What the hand holds is in the palm, unless a test puts it elsewhere
		void moveTo(const Vec3 &palm) { in.palm = palm; in.held = palm; step(); }
		void grip(bool down) { in.grip = down; step(); }
		void trigger(bool down) { in.trigger = down; step(); }
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

		// The button drops the magazine
		r.in.ejectPressed = true;
		r.step();
		CHECK(r.events & Event_Ejected);
		CHECK(r.machine.GetState() == State::NeedFresh);

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

		// Moving it forward doesn't count, and it can't go past its travel
		r.moveTo(kBolt + Vec3(3.0f, 0.0f, 0.0f));
		CHECK(r.machine.NeedsCycle());
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), r.in.boltTravel, 0.001f);
		CHECK(r.events & Event_BoltPulled);
		CHECK(!r.machine.NeedsCycle());
		CHECK(!r.machine.BoltLockedBack());
		CHECK(r.machine.BlocksFire(50, false)); // still held back

		// Pressing the grip meanwhile doesn't let go of it
		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.grip(false);
		CHECK(r.machine.GetState() == State::HoldingBolt);

		// Going forward with the hand, it follows
		r.moveTo(kBolt + Vec3(-0.5f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), 0.5f, 0.001f);
		CHECK(r.machine.BlocksFire(50, false)); // more than a tenth of the way back
		r.moveTo(kBolt + Vec3(-0.1f, 0.0f, 0.0f));
		CHECK(!r.machine.BlocksFire(50, false));

		r.trigger(false);
		CHECK(r.events & Event_BoltReleased);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK_NEAR(r.machine.BoltPull(), 0.0f, 0.001f);

		// It doesn't ask again until the weapon runs out again
		r.step();
		CHECK(!r.machine.NeedsCycle());

		// The button can also work the bolt
		r.in.clip = 0;
		r.step();
		r.in.clip = 50;
		r.step();
		CHECK(r.machine.NeedsCycle());
		r.in.ejectPressed = true;
		r.step();
		CHECK(r.events & Event_BoltPulled);
		CHECK(r.events & Event_BoltReleased);
		CHECK(r.machine.GetState() == State::Idle); // didn't drop the magazine

		// Pulled when it isn't needed, it still moves and clicks, and changes nothing
		CHECK(!r.machine.NeedsCycle());
		r.moveTo(kBolt);
		r.trigger(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.moveTo(kBolt + Vec3(-1.0f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), 1.0f, 0.001f);
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK(r.events & Event_BoltPulled);
		r.trigger(false);
		CHECK(r.events & Event_BoltReleased);
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

		// Into the weapon: the old one comes out as it goes in
		r.grip(true);
		r.moveTo(kSocket + Vec3(0.0f, 0.0f, -3.0f));
		CHECK(r.events & Event_Inserted);
		CHECK(r.events & Event_MagazineOut);
		CHECK(r.machine.GetState() == State::Idle);
		CHECK(!r.machine.MagazineIsOut());
		r.grip(false);

		// It goes in when the magazine reaches the weapon, wherever the palm is
		Rig s(7, 15, 999, false);
		s.moveTo(kWaist);
		s.grip(true);
		s.in.palm = kSocket + Vec3(0.0f, 0.0f, -6.0f); // 14 cm away
		s.in.held = kSocket + Vec3(0.0f, 0.0f, -3.0f); // the magazine sticks out of the hand towards it
		s.step();
		CHECK(s.events & Event_Inserted);

		// Full: nothing to take
		Rig full(15, 15, 999, false);
		full.moveTo(kWaist);
		full.grip(true);
		CHECK(full.machine.GetState() == State::Idle);
		CHECK(!full.machine.HandIsBusy()); // free to take an item from the body
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
		CHECK(!r.machine.BlocksFire(7, true));
		r.trigger(false);
		CHECK(r.events & Event_BoltReleased);

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

		// Full: no more
		CHECK(r.in.clip == 8);
		r.moveTo(kWaist);
		r.grip(true);
		CHECK(r.machine.GetState() == State::Idle);
		r.grip(false);

		// Dropping a shell doesn't need anything else
		r.in.clip = 5;
		r.step();
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
			r.in.ejectPressed = true;
			r.step();
			CHECK(r.machine.GetState() == State::Idle);
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
			r.in.ejectPressed = true;
			r.step();
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
			r.in.ejectPressed = true;
			r.step();
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
}

void RunReloadTests()
{
	TestMagazine();
	TestEmptyAndBolt();
	TestMagazineFromWaist();
	TestPumpEveryShot();
	TestShells();
	TestEdges();
}
