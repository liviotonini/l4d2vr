// Tests of moving the player with the body (roomscale) and with the thumbstick (movement_logic.h).
// Build and run: tests\run_tests.cmd
#include <cstdio>
#include "../L4D2VR/movement_logic.h"
#include "test_common.h"

using namespace Movement;

namespace
{
	const float kTick = 1.0f / 30.0f;
	const float kMaxSpeed = 220.0f;

	void TestCameraFollow()
	{
		printf("Movement: the camera with the body\n");
		const Vec2 head(10.0f, 0.0f);

		// The body catching up with the head: the camera stays
		Vec2 shift = CameraFollow(Vec2(10.0f, 0.0f), head);
		CHECK_NEAR(Length(shift), 0.0f, 0.001f);
		shift = CameraFollow(Vec2(4.0f, 0.0f), head);
		CHECK_NEAR(Length(shift), 0.0f, 0.001f);

		// Past the head: the camera goes on with it
		shift = CameraFollow(Vec2(15.0f, 0.0f), head);
		CHECK_NEAR(shift.x, 5.0f, 0.001f);
		CHECK_NEAR(shift.y, 0.0f, 0.001f);

		// Sideways (the thumbstick), backwards (pushed), with the head on the body: all of it
		shift = CameraFollow(Vec2(0.0f, 6.0f), head);
		CHECK_NEAR(shift.x, 0.0f, 0.001f);
		CHECK_NEAR(shift.y, 6.0f, 0.001f);
		shift = CameraFollow(Vec2(-7.0f, 0.0f), head);
		CHECK_NEAR(shift.x, -7.0f, 0.001f);
		shift = CameraFollow(Vec2(3.0f, 4.0f), Vec2());
		CHECK_NEAR(shift.x, 3.0f, 0.001f);
		CHECK_NEAR(shift.y, 4.0f, 0.001f);

		// At an angle: only the part towards the head stays
		shift = CameraFollow(Vec2(5.0f, 5.0f), head);
		CHECK_NEAR(shift.x, 0.0f, 0.001f);
		CHECK_NEAR(shift.y, 5.0f, 0.001f);

		// A little past the head (the body slowing down after a step) doesn't take the camera, more does
		shift = CameraFollow(Vec2(12.0f, 0.0f), head, 2.6f);
		CHECK_NEAR(Length(shift), 0.0f, 0.001f);
		shift = CameraFollow(Vec2(15.0f, 0.0f), head, 2.6f);
		CHECK_NEAR(shift.x, 2.4f, 0.001f);
		shift = CameraFollow(Vec2(2.0f, 0.0f), Vec2(), 2.6f);
		CHECK_NEAR(Length(shift), 0.0f, 0.001f);
		shift = CameraFollow(Vec2(0.0f, 10.0f), Vec2(), 2.6f);
		CHECK_NEAR(shift.y, 7.4f, 0.001f);
		// Backwards (pushed) it's all taken, whatever the overshoot
		shift = CameraFollow(Vec2(-7.0f, 0.0f), head, 2.6f);
		CHECK_NEAR(shift.x, -7.0f, 0.001f);
	}

	void TestStick()
	{
		printf("Movement: the thumbstick\n");
		const float deadZone = 0.15f;
		CHECK_NEAR(Length(StickShare(0.1f, 0.05f, deadZone)), 0.0f, 0.001f);
		CHECK_NEAR(Length(StickShare(0.0f, 1.0f, deadZone)), 1.0f, 0.001f);
		CHECK_NEAR(Length(StickShare(0.0f, 0.575f, deadZone)), 0.5f, 0.001f);
		// Its direction is kept, and pushed into a corner it's no more than all the way
		const Vec2 half = StickShare(-0.575f, 0.0f, deadZone);
		CHECK_NEAR(half.x, -0.5f, 0.001f);
		CHECK_NEAR(half.y, 0.0f, 0.001f);
		const Vec2 corner = StickShare(1.0f, 1.0f, deadZone);
		CHECK_NEAR(Length(corner), 1.0f, 0.001f);
		CHECK_NEAR(corner.x, corner.y, 0.001f);

		// From the slowest the game keeps up to the top speed
		CHECK_NEAR(Length(StickSpeeds(Vec2(), 50.0f, kMaxSpeed)), 0.0f, 0.001f);
		CHECK_NEAR(Length(StickSpeeds(Vec2(0.0f, 1.0f), 50.0f, kMaxSpeed)), kMaxSpeed, 0.01f);
		CHECK_NEAR(Length(StickSpeeds(Vec2(0.0f, 0.01f), 50.0f, kMaxSpeed)), 51.7f, 0.01f);
		CHECK_NEAR(StickSpeeds(Vec2(-0.5f, 0.0f), 50.0f, kMaxSpeed).x, -135.0f, 0.01f);
		CHECK_NEAR(Length(StickSpeeds(Vec2(0.0f, 0.5f), 100.0f, 85.0f)), 85.0f, 0.01f); // walking: no faster than the walk

		// To the usercmd: forward along the yaw, side to its right
		float forward, side;
		ToCommand(Vec2(10.0f, 0.0f), 0.0f, forward, side);
		CHECK_NEAR(forward, 10.0f, 0.001f);
		CHECK_NEAR(side, 0.0f, 0.001f);
		ToCommand(Vec2(0.0f, -10.0f), 0.0f, forward, side);
		CHECK_NEAR(forward, 0.0f, 0.001f);
		CHECK_NEAR(side, 10.0f, 0.001f);
		ToCommand(Vec2(0.0f, 10.0f), 90.0f, forward, side);
		CHECK_NEAR(forward, 10.0f, 0.001f);
		CHECK_NEAR(side, 0.0f, 0.001f);

		forward = 300.0f;
		side = 400.0f;
		LimitSpeed(forward, side, kMaxSpeed);
		CHECK_NEAR(sqrtf(forward * forward + side * side), kMaxSpeed, 0.01f);
		CHECK_NEAR(forward / side, 0.75f, 0.001f);
		forward = 30.0f;
		side = 40.0f;
		LimitSpeed(forward, side, kMaxSpeed);
		CHECK_NEAR(forward, 30.0f, 0.001f);
	}

	// The body walking to the head with the speed asked, a tick at a time; returns the ticks it took to stop
	int WalkToHead(Roomscale &roomscale, Vec2 &body, const Vec2 &head, const RoomscaleSettings &s, int ticks)
	{
		Vec2 moved;
		for (int i = 0; i < ticks; ++i)
		{
			const Vec2 velocity = roomscale.Update(head - body, moved, true, kMaxSpeed, kTick, s);
			if (!roomscale.Following())
				return i;
			moved = velocity * kTick;
			body = body + moved;
		}
		return ticks;
	}

	void TestRoomscale()
	{
		printf("Movement: the body walking to the head\n");
		const RoomscaleSettings s;

		// A small lean doesn't move the body
		{
			Roomscale roomscale;
			const Vec2 velocity = roomscale.Update(Vec2(3.0f, 0.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK(!roomscale.Following());
			CHECK_NEAR(Length(velocity), 0.0f, 0.001f);
		}

		// A step: the body walks to the head and stops there; leaning again inside the dead zone keeps it still
		{
			Roomscale roomscale;
			Vec2 body;
			const Vec2 head(0.0f, 13.0f); // 30 cm to the side
			const Vec2 first = roomscale.Update(head - body, Vec2(), true, kMaxSpeed, kTick, s);
			CHECK(roomscale.Following());
			CHECK_NEAR(Length(first), s.gain * 13.0f, 0.01f);
			CHECK_NEAR(first.x, 0.0f, 0.001f);
			CHECK(first.y > 0.0f);
			const int ticks = WalkToHead(roomscale, body, head, s, 200);
			CHECK(ticks < 60); // under 2 seconds
			CHECK(!roomscale.Following());
			CHECK(Length(head - body) < s.stopDistance);
			CHECK(!roomscale.Blocked());
			const Vec2 still = roomscale.Update(Vec2(0.0f, 4.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK_NEAR(Length(still), 0.0f, 0.001f);
		}

		// Slow to get going (the game's acceleration from a standstill): not taken as blocked
		{
			Roomscale roomscale;
			Vec2 body, moved;
			const Vec2 head(0.0f, 20.0f);
			RoomscaleSettings slowest;
			slowest.minimumSpeed = 75.0f;
			for (int i = 0; i < 60 && roomscale.Update(head - body, moved, true, kMaxSpeed, kTick, slowest).y > 0.0f; ++i)
			{
				const float share = i < 12 ? 0.15f : 0.6f;
				moved = Vec2(0.0f, std::min(75.0f * share * kTick, Length(head - body)));
				body = body + moved;
				CHECK(!roomscale.Blocked());
			}
			CHECK(!roomscale.Blocked());
		}

		// Getting past the head while slowing down: it doesn't walk back
		{
			Roomscale roomscale;
			roomscale.Update(Vec2(10.0f, 0.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK(roomscale.Following());
			const Vec2 velocity = roomscale.Update(Vec2(-2.0f, 0.0f), Vec2(12.0f, 0.0f), true, kMaxSpeed, kTick, s);
			CHECK(!roomscale.Following());
			CHECK_NEAR(Length(velocity), 0.0f, 0.001f);
		}

		// Close, no slower than the game's movement keeps up
		{
			Roomscale roomscale;
			RoomscaleSettings slowest;
			slowest.minimumSpeed = 60.0f;
			const Vec2 velocity = roomscale.Update(Vec2(6.0f, 0.0f), Vec2(), true, kMaxSpeed, kTick, slowest);
			CHECK_NEAR(Length(velocity), 60.0f, 0.01f);
		}

		// Far away it's no faster than the game lets the player run
		{
			Roomscale roomscale;
			const Vec2 velocity = roomscale.Update(Vec2(100.0f, 0.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK_NEAR(Length(velocity), kMaxSpeed, 0.01f);
		}

		// Walking around the room: the head keeps going, the body follows without being taken as blocked
		{
			Roomscale roomscale;
			Vec2 body, head, moved;
			for (int i = 0; i < 150; ++i)
			{
				head = head + Vec2(50.0f * kTick, 0.0f); // about 1.2 m/s
				const Vec2 velocity = roomscale.Update(head - body, moved, true, kMaxSpeed, kTick, s);
				moved = velocity * kTick;
				body = body + moved;
			}
			CHECK(!roomscale.Blocked());
			CHECK(Length(head - body) < 12.0f);
		}

		// Against a wall: it stops asking, until the head moves
		{
			Roomscale roomscale;
			const Vec2 head(10.0f, 0.0f);
			bool blocked = false;
			for (int i = 0; i < 40 && !blocked; ++i)
			{
				roomscale.Update(head, Vec2(), true, kMaxSpeed, kTick, s);
				blocked = roomscale.Blocked();
			}
			CHECK(blocked);
			Vec2 velocity = roomscale.Update(head + Vec2(0.0f, 1.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK_NEAR(Length(velocity), 0.0f, 0.001f);
			CHECK(roomscale.Blocked());
			velocity = roomscale.Update(head + Vec2(0.0f, 4.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK(!roomscale.Blocked());
			CHECK(Length(velocity) > 0.0f);
		}

		// In the air, on a ladder: nothing, and it starts again from the dead zone after
		{
			Roomscale roomscale;
			roomscale.Update(Vec2(10.0f, 0.0f), Vec2(), true, kMaxSpeed, kTick, s);
			CHECK(roomscale.Following());
			const Vec2 velocity = roomscale.Update(Vec2(10.0f, 0.0f), Vec2(), false, kMaxSpeed, kTick, s);
			CHECK_NEAR(Length(velocity), 0.0f, 0.001f);
			CHECK(!roomscale.Following());
		}
	}
}

void RunMovementTests()
{
	TestCameraFollow();
	TestStick();
	TestRoomscale();
}
