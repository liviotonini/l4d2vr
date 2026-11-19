// Tests of where things are carried on the body (body_layout.h).
// Build and run: tests\run_tests.cmd
#include "../L4D2VR/body_layout.h"
#include "test_common.h"

using namespace HandMath;
using namespace BodyLayout;

namespace
{
	void TestHolsterStanding()
	{
		const float height = 1.68f;
		const Holster holster = HolsterOnThigh(height, kEyeHeight * height, false);
		// Below the eyes by what's between them and the holster's height, on the right, pointing straight down
		CHECK_NEAR(holster.point.z, -(kEyeHeight - kHolsterHeight) * height, 0.001f);
		CHECK_NEAR(holster.point.y, -kHolsterSide * height, 0.001f);
		CHECK_NEAR(holster.point.x, 0.0f, 0.001f);
		CHECK_NEAR(holster.barrel.z, -1.0f, 0.001f);
		CHECK_NEAR(holster.slide.x, 1.0f, 0.001f);

		// Where the user put it: 0.4977 of the height below the eyes at first, then 5 cm higher twice
		CHECK(holster.point.z > -0.4977f * height + 0.10f);

		// Left handed: on the left
		CHECK_NEAR(HolsterOnThigh(height, kEyeHeight * height, true).point.y, kHolsterSide * height, 0.001f);

		// Taller players carry it lower, the same share of their height
		CHECK_NEAR(HolsterOnThigh(1.9f, kEyeHeight * 1.9f, false).point.z, -(kEyeHeight - kHolsterHeight) * 1.9f, 0.001f);

		// Standing a bit lower than the average eyes, or a floor a bit off: still standing
		const Holster almost = HolsterOnThigh(height, kEyeHeight * height - 0.06f, false);
		CHECK_NEAR(almost.point.z, holster.point.z, 0.001f);
		CHECK_NEAR(almost.barrel.z, -1.0f, 0.001f);
	}

	void TestHolsterCrouching()
	{
		const float height = 1.68f, standingEyes = kEyeHeight * height;
		const Holster standing = HolsterOnThigh(height, standingEyes, false);

		float previousZ = standing.point.z, previousBarrelZ = standing.barrel.z;
		for (int step = 1; step <= 7; ++step)
		{
			const float drop = step * 0.1f;
			const Holster crouched = HolsterOnThigh(height, standingEyes - drop, false);
			// The hips go down less than the head: the holster gets closer to the eyes, and lower in the room
			CHECK(crouched.point.z > previousZ);
			CHECK(crouched.point.z - drop < standing.point.z);
			// The thigh turns forward with the pistol on it: the knee's end comes up
			CHECK(crouched.barrel.z > previousBarrelZ);
			CHECK(crouched.barrel.x > 0.0f);
			CHECK_NEAR(Length(crouched.barrel), 1.0f, 0.001f);
			CHECK_NEAR(Dot(crouched.barrel, crouched.slide), 0.0f, 0.001f);
			previousZ = crouched.point.z;
			previousBarrelZ = crouched.barrel.z;
		}

		// A deep squat: the thigh about level, the pistol pointing forward, at about the height of the knee
		const Holster squat = HolsterOnThigh(height, standingEyes - 0.6f, false);
		CHECK(squat.barrel.x > 0.95f);
		const float squatAboveFloor = standingEyes - 0.6f + squat.point.z;
		CHECK(squatAboveFloor > (kKneeHeight - 0.1f) * height && squatAboveFloor < (kKneeHeight + 0.1f) * height);
		// Further down it doesn't fold back up
		CHECK(HolsterOnThigh(height, 0.3f, false).barrel.x > 0.9f);
	}

	void TestItemSpots()
	{
		// The defaults: where the user (1.68 m) put them, pills on the left, the grenade on the right
		const Vec3 pills = ItemSpotOffset(DefaultItemSpot(0, 1.68f), false);
		const Vec3 grenade = ItemSpotOffset(DefaultItemSpot(1, 1.68f), false);
		CHECK_NEAR(pills.x, 0.03f, 0.001f);
		CHECK_NEAR(pills.y, 0.13f, 0.001f);
		CHECK_NEAR(pills.z, -0.35f, 0.001f);
		CHECK_NEAR(grenade.y, -0.18f, 0.001f);
		CHECK_NEAR(grenade.z, -0.29f, 0.001f);

		// The side is towards the weapon hand: mirrored for left handed players
		CHECK_NEAR(ItemSpotOffset(DefaultItemSpot(1, 1.68f), true).y, 0.18f, 0.001f);

		// Lower for taller players
		CHECK(ItemSpotOffset(DefaultItemSpot(0, 1.9f), false).z < pills.z);
	}

	void TestBodyYaw()
	{
		// A small turn of the head: the body follows slowly, not at once
		float body = FollowBodyYaw(0.0f, 30.0f, false, 0.011f);
		CHECK(body > 0.0f && body < 1.0f);
		// Staying turned, it catches up in a few seconds
		for (int frame = 0; frame < 90 * 8; ++frame)
			body = FollowBodyYaw(body, 30.0f, false, 0.011f);
		CHECK_NEAR(body, 30.0f, 1.0f);

		// Looking down at the body, the head turns on its own: what's on the body stays put
		body = 0.0f;
		for (int frame = 0; frame < 90 * 5; ++frame)
			body = FollowBodyYaw(body, 40.0f, true, 0.011f);
		CHECK_NEAR(body, 0.0f, 0.001f);

		// More than 45 degrees, the body turns too, at once, and keeps the head within 45 of it
		CHECK_NEAR(FollowBodyYaw(0.0f, 100.0f, true, 0.011f), 55.0f, 0.001f);
		CHECK_NEAR(FollowBodyYaw(0.0f, -100.0f, true, 0.011f), -55.0f, 0.001f);

		// Across -180/180 it takes the short way
		CHECK_NEAR(FollowBodyYaw(170.0f, -130.0f, true, 0.011f), -175.0f, 0.001f);
		body = FollowBodyYaw(170.0f, -170.0f, false, 1.0f);
		CHECK(body > 170.0f || body < -170.0f);

		// A long pause between frames doesn't make it jump
		CHECK(fabsf(FollowBodyYaw(0.0f, 30.0f, false, 5.0f)) < 3.0f);
	}

	void TestNeedle()
	{
		// The body: a line from (0, 0, 0) down to z = -10, units. The needle 3 units in front of its middle.
		const Vec3 top(0.0f, 0.0f, 0.0f);
		const Vec3 tip(3.0f, 0.0f, -5.0f);
		CHECK(NeedleAtBody(tip, Vec3(-1.0f, 0.0f, 0.0f), top, -10.0f, 5.0f, 0.3f));
		// Pointing away, or along the body, it isn't stuck in
		CHECK(!NeedleAtBody(tip, Vec3(1.0f, 0.0f, 0.0f), top, -10.0f, 5.0f, 0.3f));
		CHECK(!NeedleAtBody(tip, Vec3(0.0f, 0.0f, -1.0f), top, -10.0f, 5.0f, 0.3f));
		// Pointing at it a bit down, like into the thigh: yes
		CHECK(NeedleAtBody(tip, Normalized(Vec3(-1.0f, 0.0f, -0.8f)), top, -10.0f, 5.0f, 0.3f));
		// Too far
		CHECK(!NeedleAtBody(Vec3(6.0f, 0.0f, -5.0f), Vec3(-1.0f, 0.0f, 0.0f), top, -10.0f, 5.0f, 0.3f));
		// Above the chest or below the thighs, measured from the ends of the line
		CHECK(NeedleAtBody(Vec3(0.0f, 2.0f, 2.0f), Normalized(Vec3(0.0f, -1.0f, -1.0f)), top, -10.0f, 5.0f, 0.3f));
		CHECK(!NeedleAtBody(Vec3(3.0f, 0.0f, -16.0f), Vec3(-1.0f, 0.0f, 0.0f), top, -10.0f, 5.0f, 0.3f));
		// The bottom above the top (crouched far down): the chest is still there
		CHECK(NeedleAtBody(Vec3(3.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f), top, 2.0f, 5.0f, 0.3f));
	}

	void TestTouchingPlayers()
	{
		// The game's scale: 43.2 units per meter. A player standing at (100, 0, 0), another one crouched at (0, 100, 0).
		const float scale = 43.2f;
		const Vec3 feet[2] = { Vec3(100.0f, 0.0f, 0.0f), Vec3(0.0f, 100.0f, 0.0f) };
		const bool crouched[2] = { false, true };

		// A hand 0.2 m in front of the chest of the one standing touches it; 0.4 m away it doesn't
		CHECK(TouchedPlayer(Vec3(100.0f + 0.2f * scale, 0.0f, 50.0f), feet, crouched, 2, scale) == 0);
		CHECK(TouchedPlayer(Vec3(100.0f + 0.4f * scale, 0.0f, 50.0f), feet, crouched, 2, scale) == -1);
		// On the head, yes; above it, or at the feet, no
		CHECK(TouchedPlayer(Vec3(100.0f, 5.0f, 70.0f), feet, crouched, 2, scale) == 0);
		CHECK(TouchedPlayer(Vec3(100.0f, 0.0f, 66.0f + 0.4f * scale), feet, crouched, 2, scale) == -1);
		CHECK(TouchedPlayer(Vec3(100.0f, 0.0f, 1.0f), feet, crouched, 2, scale) == -1);

		// Where a standing player's chest is, the one crouched has nothing
		CHECK(TouchedPlayer(Vec3(0.0f, 100.0f, 60.0f), feet, crouched, 2, scale) == -1);
		CHECK(TouchedPlayer(Vec3(0.0f, 100.0f + 0.2f * scale, 35.0f), feet, crouched, 2, scale) == 1);

		// Between two players the nearer one; nobody around, none
		const Vec3 close[2] = { Vec3(0.0f, 0.0f, 0.0f), Vec3(20.0f, 0.0f, 0.0f) };
		const bool standing[2] = { false, false };
		CHECK(TouchedPlayer(Vec3(8.0f, 0.0f, 40.0f), close, standing, 2, scale) == 0);
		CHECK(TouchedPlayer(Vec3(12.0f, 0.0f, 40.0f), close, standing, 2, scale) == 1);
		CHECK(TouchedPlayer(Vec3(8.0f, 0.0f, 40.0f), close, standing, 0, scale) == -1);

		// The distance to a body line: from its ends above and below it
		CHECK_NEAR(DistanceToBodyLine(Vec3(3.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 0.0f), -10.0f), 3.0f, 0.001f);
		CHECK_NEAR(DistanceToBodyLine(Vec3(0.0f, 0.0f, 4.0f), Vec3(0.0f, 0.0f, 0.0f), -10.0f), 4.0f, 0.001f);
		CHECK_NEAR(DistanceToBodyLine(Vec3(0.0f, 0.0f, -13.0f), Vec3(0.0f, 0.0f, 0.0f), -10.0f), 3.0f, 0.001f);
	}
}

namespace
{
	// Turned without mirroring, every axis scaled the same
	void CheckProperScaled(const Mat34 &m, float scale)
	{
		CHECK_NEAR(Length(Axis(m, 0)), scale, 0.001f);
		CHECK_NEAR(Length(Axis(m, 1)), scale, 0.001f);
		CHECK_NEAR(Length(Axis(m, 2)), scale, 0.001f);
		CHECK(Dot(Cross(Axis(m, 0), Axis(m, 1)), Axis(m, 2)) > 0.0f);
	}

	// The game's melee world models, measured from their vertices in the frame of their weapon bone (units)
	void TestMeleeOnThigh()
	{
		const float maxLength = 13.0f, maxScale = 0.75f;

		// The katana: the blade (y) down along the thigh with the handle up, the edge (z, wider than x) forward, as long
		// as the place
		const Vec3 katanaMin(-1.3f, -9.0f, -2.0f), katanaMax(1.3f, 29.6f, 1.7f);
		const Mat34 katana = HungOnThigh(katanaMin, katanaMax, maxLength, maxScale);
		const float katanaScale = maxLength / 38.6f;
		CheckProperScaled(katana, katanaScale);
		CHECK(TransformPoint(katana, Vec3(0.0f, 29.6f, 0.0f)).y > 0.0f);
		CHECK(TransformPoint(katana, Vec3(0.0f, -9.0f, 0.0f)).y < 0.0f);
		CHECK_NEAR(TransformPoint(katana, Vec3(0.0f, 29.6f, 0.0f)).y - TransformPoint(katana, Vec3(0.0f, -9.0f, 0.0f)).y, maxLength, 0.01f);
		CHECK_NEAR(fabsf(Axis(katana, 2).z), katanaScale, 0.001f);
		CHECK_NEAR(fabsf(Axis(katana, 0).x), katanaScale, 0.001f);
		// Its middle in the middle of the place
		const Vec3 middle = TransformPoint(katana, (katanaMin + katanaMax) * 0.5f);
		CHECK_NEAR(Length(middle), 0.0f, 0.001f);

		// The knife is shorter than the place: only made smaller by the most it's ever made smaller
		CheckProperScaled(HungOnThigh(Vec3(-0.6f, -1.7f, -1.5f), Vec3(0.7f, 12.7f, 1.1f), maxLength, maxScale), maxScale);

		// The chainsaw's bar goes along -z from where it's held: the bar down, the handle up
		const Mat34 chainsaw = HungOnThigh(Vec3(-5.8f, -7.5f, -38.9f), Vec3(4.0f, 3.5f, 4.4f), maxLength, maxScale);
		CheckProperScaled(chainsaw, maxLength / 43.3f);
		CHECK(TransformPoint(chainsaw, Vec3(0.0f, 0.0f, -38.9f)).y > 0.0f);
		CHECK_NEAR(fabsf(Axis(chainsaw, 2).y), maxLength / 43.3f, 0.001f);

		// The riot shield is thin along z: that side against the leg, its face along it
		const Mat34 shield = HungOnThigh(Vec3(-3.6f, -19.8f, -0.8f), Vec3(16.6f, 20.7f, 4.2f), maxLength, maxScale);
		CheckProperScaled(shield, maxLength / 40.5f);
		CHECK_NEAR(fabsf(Axis(shield, 2).x), maxLength / 40.5f, 0.001f);
		CHECK_NEAR(fabsf(Axis(shield, 0).z), maxLength / 40.5f, 0.001f);

		// The frying pan: the handle up, the pan down, its flat face against the leg
		const Mat34 pan = HungOnThigh(Vec3(-0.2f, -2.4f, -6.2f), Vec3(1.8f, 15.9f, 6.2f), maxLength, maxScale);
		CHECK(TransformPoint(pan, Vec3(0.0f, 15.9f, 0.0f)).y > 0.0f);
		CHECK_NEAR(fabsf(Axis(pan, 0).x), maxLength / 18.3f, 0.001f);

		// A box with no size doesn't break it
		CHECK(std::isfinite(HungOnThigh(Vec3(), Vec3(), maxLength, maxScale).m[0][0]));
	}
}

void RunBodyTests()
{
	printf("Body layout\n");
	TestMeleeOnThigh();
	TestHolsterStanding();
	TestHolsterCrouching();
	TestItemSpots();
	TestBodyYaw();
	TestNeedle();
	TestTouchingPlayers();
}
