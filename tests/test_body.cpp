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

		// Higher than before (it was 0.4977 of the height below the eyes)
		CHECK(holster.point.z > -0.4977f * height + 0.05f);

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
		// The defaults of a player of 1.8 m: pills on the left, the grenade on the right
		const Vec3 pills = ItemSpotOffset(DefaultItemSpot(0, 1.8f), false);
		const Vec3 grenade = ItemSpotOffset(DefaultItemSpot(1, 1.8f), false);
		CHECK_NEAR(pills.x, 0.11f, 0.001f);
		CHECK_NEAR(pills.y, 0.13f, 0.001f);
		CHECK_NEAR(pills.z, -0.37f, 0.001f);
		CHECK_NEAR(grenade.y, -0.16f, 0.001f);
		CHECK_NEAR(grenade.z, -0.34f, 0.001f);

		// The side is towards the weapon hand: mirrored for left handed players
		CHECK_NEAR(ItemSpotOffset(DefaultItemSpot(1, 1.8f), true).y, 0.16f, 0.001f);

		// Lower for shorter players
		CHECK(ItemSpotOffset(DefaultItemSpot(0, 1.6f), false).z > pills.z);
	}
}

void RunBodyTests()
{
	printf("Body layout\n");
	TestHolsterStanding();
	TestHolsterCrouching();
	TestItemSpots();
}
