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
			in.clip = clip;
			in.capacity = capacity;
			in.reserve = reserve;
			in.shells = shells;
			in.hasBolt = bolt;
			step();
		}

		void step() { events = machine.Update(in); in.ejectPressed = false; }
		void moveTo(const Vec3 &palm) { in.palm = palm; step(); }
		void grip(bool down) { in.grip = down; step(); }
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

		// Grab it and pull only a little
		r.moveTo(kBolt);
		r.grip(true);
		CHECK(r.machine.GetState() == State::HoldingBolt);
		r.moveTo(kBolt + Vec3(-0.5f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), 0.5f, 0.001f);
		CHECK(r.machine.NeedsCycle());

		// Moving it forward doesn't count, and it can't go past its travel
		r.moveTo(kBolt + Vec3(3.0f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), 0.0f, 0.001f);
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK_NEAR(r.machine.BoltPull(), r.in.boltTravel, 0.001f);
		CHECK(r.events & Event_BoltPulled);
		CHECK(!r.machine.NeedsCycle());
		CHECK(!r.machine.BlocksFire(50, false));

		r.grip(false);
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
		CHECK(r.machine.GetState() == State::Idle); // didn't drop the magazine
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
		r.moveTo(kBolt);
		r.grip(true);
		r.moveTo(kBolt + Vec3(-9.0f, 0.0f, 0.0f));
		CHECK(r.events & Event_BoltPulled);
		CHECK(!r.machine.BlocksFire(1, true));
		r.grip(false);
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
	TestShells();
	TestEdges();
}
