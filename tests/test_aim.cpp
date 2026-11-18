// Tests of the spread and kick of the guns aimed with the controller (aim_logic.h).
// Build and run: tests\run_tests.cmd
#include <cstdio>
#include "../L4D2VR/aim_logic.h"
#include "test_common.h"

using namespace Aim;

namespace
{
	// The speeds the game is asked for (vr.h): the walk toggle, the slowest of the thumbstick, running
	const float kWalkSpeed = 85.0f;
	const float kSlowestStickSpeed = 75.0f;
	const float kRunSpeed = 220.0f;

	void TestMovementSpread()
	{
		printf("Aim: the spread follows the speed\n");
		const GunScript &rifle = *FindGun(Rifle);

		// Still: the least spread, crouched less
		CHECK_NEAR(MovementSpread(rifle, 0.0f, false, true), 0.4f, 0.0001f);
		CHECK_NEAR(MovementSpread(rifle, 0.0f, true, true), 0.05f, 0.0001f);
		// Running: three quarters of the way to MaxMovementSpread, not more past 220
		CHECK_NEAR(MovementSpread(rifle, kRunSpeed, false, true), 0.4f + 4.6f * 0.75f, 0.0001f);
		CHECK_NEAR(MovementSpread(rifle, 300.0f, false, true), MovementSpread(rifle, kRunSpeed, false, true), 0.0001f);
		// Crouched and moving: half of the way at most
		CHECK_NEAR(MovementSpread(rifle, kRunSpeed, true, true), 0.05f + 4.95f * 0.5f, 0.0001f);
		// In the air: MaxMovementSpread
		CHECK_NEAR(MovementSpread(rifle, 0.0f, false, false), 5.0f, 0.0001f);

		// The thumbstick: slower aims better, in a straight line; its slowest is at least as good as walking
		const float slowest = MovementSpread(rifle, kSlowestStickSpeed, false, true);
		const float walking = MovementSpread(rifle, kWalkSpeed, false, true);
		const float half = MovementSpread(rifle, (kSlowestStickSpeed + kRunSpeed) * 0.5f, false, true);
		const float running = MovementSpread(rifle, kRunSpeed, false, true);
		CHECK(slowest <= walking);
		CHECK(walking < half && half < running);
		CHECK_NEAR(half, (slowest + running) * 0.5f, 0.0001f);
	}

	void TestOneHanded()
	{
		printf("Aim: a long gun with one hand\n");
		int longGuns = 0;
		const int all[] = { Pistol, Magnum, Uzi, Mac10, Mp5, PumpShotgun, ChromeShotgun, AutoShotgun, Spas, Rifle, Scar, Ak47, Sg552,
			M60, HuntingRifle, MilitarySniper, Awp, Scout, GrenadeLauncher };
		for (int id : all)
		{
			CHECK(FindGun(id) != nullptr);
			if (NeedsBothHands(id))
				++longGuns;
		}
		// Every primary weapon but the three SMGs; not the pistols
		CHECK(longGuns == 14);
		CHECK(!NeedsBothHands(Uzi) && !NeedsBothHands(Mac10) && !NeedsBothHands(Mp5));
		CHECK(!NeedsBothHands(Pistol) && !NeedsBothHands(Magnum));
		CHECK(NeedsBothHands(Rifle) && NeedsBothHands(PumpShotgun) && NeedsBothHands(Awp) && NeedsBothHands(M60));
		CHECK(!NeedsBothHands(0) && FindGun(19) == nullptr); // nothing, a melee weapon

		// Standing still with one hand spreads like running with both
		const GunScript &rifle = *FindGun(Rifle);
		CHECK_NEAR(OneHandedSpread(rifle, MovementSpread(rifle, 0.0f, false, true)), MovementSpread(rifle, kRunSpeed, false, true), 0.0001f);
		// And it adds to whatever else spreads it
		CHECK_NEAR(OneHandedSpread(rifle, 2.0f) - 2.0f, 4.6f * 0.75f, 0.0001f);
		// A sniper rifle: a lot worse than its tenth of a degree
		const GunScript &awp = *FindGun(Awp);
		CHECK(OneHandedSpread(awp, awp.minStanding) > 20.0f * awp.minStanding);

		// A shot: the gun's spread with the game's on, none without; one hand adds to either, but not on an SMG
		CHECK_NEAR(ShotSpread(rifle, 1.5f, true, false), 1.5f, 0.0001f);
		CHECK_NEAR(ShotSpread(rifle, 1.5f, false, false), 0.0f, 0.0001f);
		CHECK_NEAR(ShotSpread(rifle, 1.5f, true, true), OneHandedSpread(rifle, 1.5f), 0.0001f);
		CHECK_NEAR(ShotSpread(rifle, 1.5f, false, true), OneHandedSpread(rifle, 0.0f), 0.0001f);
		CHECK_NEAR(ShotSpread(*FindGun(Uzi), 1.5f, true, true), 1.5f, 0.0001f);
		// The gun's spread unknown (the game's code not found): none of it
		CHECK_NEAR(ShotSpread(rifle, -1.0f, true, false), 0.0f, 0.0001f);
	}

	void TestSpreadAngles()
	{
		printf("Aim: the random part of a shot\n");
		float pitch = 0.0f, yaw = 0.0f, again = 0.0f, againYaw = 0.0f;
		SpreadAngles(12345, 2.0f, pitch, yaw);
		SpreadAngles(12345, 2.0f, again, againYaw);
		CHECK(pitch == again && yaw == againYaw);

		SpreadAngles(777, 0.0f, pitch, yaw);
		CHECK(pitch == 0.0f && yaw == 0.0f);

		// Within the spread, both ways, spread over all of it, and other seeds give other shots
		const int count = 4000;
		float lowest = 0.0f, highest = 0.0f, sum = 0.0f;
		int quarters[4] = {};
		int same = 0;
		float lastPitch = 99.0f;
		for (int seed = 1; seed <= count; ++seed)
		{
			SpreadAngles(seed, 3.0f, pitch, yaw);
			lowest = std::min(lowest, std::min(pitch, yaw));
			highest = std::max(highest, std::max(pitch, yaw));
			sum += pitch + yaw;
			++quarters[std::clamp((int)((pitch + 3.0f) / 1.5f), 0, 3)];
			if (pitch == lastPitch)
				++same;
			lastPitch = pitch;
		}
		CHECK(lowest >= -3.0f && highest <= 3.0f);
		CHECK(lowest < -2.9f && highest > 2.9f);
		CHECK(fabsf(sum / (2.0f * count)) < 0.1f);
		for (int quarter : quarters)
			CHECK(quarter > count / 4 - count / 20 && quarter < count / 4 + count / 20);
		CHECK(same == 0);
		// The seeds of usercmds are big numbers, negatives too in a cast
		SpreadAngles(0x7FFFFFFF, 1.0f, pitch, yaw);
		CHECK(fabsf(pitch) <= 1.0f && fabsf(yaw) <= 1.0f);
		SpreadAngles(-5, 1.0f, pitch, yaw);
		CHECK(fabsf(pitch) <= 1.0f && fabsf(yaw) <= 1.0f);
	}

	void TestKick()
	{
		printf("Aim: the kick of a long gun with one hand\n");
		const GunScript &rifle = *FindGun(Rifle);
		Kick kick;
		CHECK(!kick.Active());
		kick.Shot(rifle, 1);
		CHECK_NEAR(kick.pitch, 1.2f * kKickPerPunch, 0.0001f);
		CHECK(fabsf(kick.yaw) <= kick.pitch * kKickSideShare + 0.0001f);
		CHECK(kick.Active());

		// It comes back: most of it in half a second, all of it in a second (at 90 frames a second)
		for (int frame = 0; frame < 45; ++frame)
			kick.Update(1.0f / 90.0f);
		CHECK(kick.pitch < 0.1f * 1.2f * kKickPerPunch);
		for (int frame = 0; frame < 45; ++frame)
			kick.Update(1.0f / 90.0f);
		CHECK(!kick.Active());

		// Firing full auto it climbs, to a few shots' worth (the rifle: 11 shots a second), never past the limit
		kick = Kick();
		float top = 0.0f;
		for (int shot = 0; shot < 30; ++shot)
		{
			kick.Shot(rifle, shot + 1);
			top = std::max(top, kick.pitch);
			for (int frame = 0; frame < 8; ++frame)
				kick.Update(0.0875f / 8.0f);
		}
		CHECK(top > 2.0f * 1.2f * kKickPerPunch);
		CHECK(top < kKickMost);
		const GunScript &pump = *FindGun(PumpShotgun);
		for (int shot = 0; shot < 30; ++shot)
			kick.Shot(pump, shot);
		CHECK_NEAR(kick.pitch, kKickMost, 0.0001f);
		CHECK(fabsf(kick.yaw) <= kKickMost);

		// The same however the frames come
		Kick fast, slow;
		fast.Shot(rifle, 5);
		slow.Shot(rifle, 5);
		for (int frame = 0; frame < 12; ++frame)
			fast.Update(0.1f / 12.0f);
		slow.Update(0.1f);
		CHECK_NEAR(fast.pitch, slow.pitch, 0.001f);
		// A pause of the game (no time, or less than none) doesn't move it
		const float before = slow.pitch;
		slow.Update(0.0f);
		slow.Update(-1.0f);
		CHECK_NEAR(slow.pitch, before, 0.0001f);
	}
}

void RunAimTests()
{
	TestMovementSpread();
	TestOneHanded();
	TestSpreadAngles();
	TestKick();
}
