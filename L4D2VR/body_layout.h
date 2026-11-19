#pragma once
#include <algorithm>
#include <cmath>
#include "hand_math.h"

// Where things are carried on the body, from the player's height. No game types, so it's testable (tests/).
// The body frame is the one of body_items.cpp: x forward, y left, z up, meters, from where the eyes are with the head
// looking straight ahead. "Side" is towards the weapon hand.
namespace BodyLayout
{
	using HandMath::Vec3;

	// Average proportions of an adult, per meter of height
	constexpr float kEyeHeight = 0.936f;
	constexpr float kHipHeight = 0.53f; // the hip joint
	constexpr float kKneeHeight = 0.285f;

	// The pistol's grip on the thigh of the weapon hand, standing: height above the floor and distance to the side. Where
	// the user put it for a height of 1.68 m (5 cm up and 5 cm in from 0.475 and 0.1535).
	constexpr float kHolsterHeight = 0.505f;
	constexpr float kHolsterSide = 0.1237f;

	// Crouching: how much of the head's drop the hips follow (the back leans forward for the rest) and how far back
	// they go for it. The first centimeters of the drop are ignored: standing straight or not, a floor a bit off.
	constexpr float kHipDropPerHeadDrop = 0.8f;
	constexpr float kHipBackPerHipDrop = 0.3f;
	constexpr float kCrouchDeadZone = 0.08f;

	// The pistol on the thigh, in the body frame
	struct Holster
	{
		Vec3 point;  // the middle of the grip
		Vec3 barrel; // along the thigh, towards the knee
		Vec3 slide;  // the other way from the grip, which sticks out backwards when standing
	};

	// `eyeHeight` is how high the eyes are now, meters above the floor. The knee stays where it is while the hips go
	// down, so the thigh turns forward with the pistol on it, up to a bit past level.
	inline Holster HolsterOnThigh(float height, float eyeHeight, bool leftHanded)
	{
		const float headDrop = std::max(0.0f, kEyeHeight * height - eyeHeight - kCrouchDeadZone);
		const float hipDrop = headDrop * kHipDropPerHeadDrop;
		const float thigh = (kHipHeight - kKneeHeight) * height;
		const float cosine = std::clamp(1.0f - hipDrop / thigh, -0.3f, 1.0f);
		const float sine = sqrtf(1.0f - cosine * cosine);

		const float side = (leftHanded ? 1.0f : -1.0f) * kHolsterSide * height;
		const Vec3 hip(-hipDrop * kHipBackPerHipDrop, side, -(kEyeHeight - kHipHeight) * height + headDrop - hipDrop);

		Holster holster;
		holster.barrel = Vec3(sine, 0.0f, -cosine);
		holster.slide = Vec3(cosine, 0.0f, sine);
		holster.point = hip + holster.barrel * ((kHipHeight - kHolsterHeight) * height);
		return holster;
	}

	// A melee weapon on the thigh, smaller to fit there. `boxMin`/`boxMax` is its extent in the frame of the bone it's
	// held by (any units). Its longest side goes along the thigh with the end farther from that bone down (the blade or
	// the head, the handle up), its next longest forward, its thinnest against the leg. Returns where that bone goes in
	// the thigh's frame (x out of the leg, y down along it, z forward), scaled to at most `maxLength` long and at most
	// `maxScale`, with the middle of the box at the frame's origin.
	inline HandMath::Mat34 HungOnThigh(const Vec3 &boxMin, const Vec3 &boxMax, float maxLength, float maxScale)
	{
		const float low[3] = { boxMin.x, boxMin.y, boxMin.z }, high[3] = { boxMax.x, boxMax.y, boxMax.z };
		int longest = 0, thinnest = 0;
		for (int axis = 1; axis < 3; ++axis)
		{
			if (high[axis] - low[axis] > high[longest] - low[longest])
				longest = axis;
			if (high[axis] - low[axis] < high[thinnest] - low[thinnest])
				thinnest = axis;
		}
		if (thinnest == longest)
			thinnest = (longest + 1) % 3;
		const int middle = 3 - longest - thinnest;

		const float length = high[longest] - low[longest];
		const float scale = length > 1e-3f ? std::min(maxScale, maxLength / length) : maxScale;

		// Where each of the bone's axes goes, turned without mirroring
		Vec3 axes[3];
		axes[longest] = Vec3(0.0f, high[longest] >= -low[longest] ? 1.0f : -1.0f, 0.0f);
		axes[middle] = Vec3(0.0f, 0.0f, 1.0f);
		axes[thinnest] = Vec3(1.0f, 0.0f, 0.0f);
		if (HandMath::Dot(HandMath::Cross(axes[0], axes[1]), axes[2]) < 0.0f)
			axes[thinnest] = axes[thinnest] * -1.0f;

		HandMath::Mat34 placed = HandMath::FromAxes(axes[0] * scale, axes[1] * scale, axes[2] * scale);
		const Vec3 center = (boxMin + boxMax) * 0.5f;
		HandMath::SetAxis(placed, 3, HandMath::RotateVector(placed, center) * -1.0f);
		return placed;
	}

	// Where the pills (0) and the grenade (1) are when config.txt doesn't say: on the chest, where the user put them for a
	// height of 1.68 m (0.35 and 0.29 m below the eyes). Meters from the eyes.
	struct ItemSpot
	{
		float forward, side, below;
	};

	inline ItemSpot DefaultItemSpot(int spot, float height)
	{
		return spot == 0 ? ItemSpot{ 0.03f, -0.13f, 0.2083f * height } : ItemSpot{ 0.03f, 0.18f, 0.1726f * height };
	}

	inline Vec3 ItemSpotOffset(const ItemSpot &spot, bool leftHanded)
	{
		return Vec3(spot.forward, (leftHanded ? 1.0f : -1.0f) * spot.side, -spot.below);
	}

	// Which way the body faces, degrees. Only the head is tracked: it turns up to kHeadTurnMax on its own, and the body
	// follows it slowly while it stays turned. Looking down at the body doesn't turn it, so what's on it stays put while
	// the head turns to see it.
	constexpr float kHeadTurnMax = 45.0f;
	constexpr float kBodyFollowSeconds = 1.5f;

	inline float WrapDegrees(float degrees)
	{
		return degrees - 360.0f * floorf((degrees + 180.0f) / 360.0f);
	}

	inline float FollowBodyYaw(float bodyYaw, float headYaw, bool lookingDown, float deltaSeconds)
	{
		const float turned = WrapDegrees(headYaw - bodyYaw);
		if (fabsf(turned) > kHeadTurnMax)
			bodyYaw += turned - (turned > 0.0f ? kHeadTurnMax : -kHeadTurnMax);
		else if (!lookingDown)
			bodyYaw += turned * std::min(1.0f, std::clamp(deltaSeconds, 0.0f, 0.1f) / kBodyFollowSeconds);
		return WrapDegrees(bodyYaw);
	}

	// From `point` to the nearest point of a body around the vertical line from `top` down to the height `bottomZ`. Any
	// units.
	inline Vec3 ToBodyLine(const Vec3 &point, const Vec3 &top, float bottomZ)
	{
		const float down = std::max(top.z - bottomZ, 0.0f);
		const float along = std::clamp(top.z - point.z, 0.0f, down);
		return Vec3(top.x, top.y, top.z - along) - point;
	}

	inline float DistanceToBodyLine(const Vec3 &point, const Vec3 &top, float bottomZ)
	{
		return HandMath::Length(ToBodyLine(point, top, bottomZ));
	}

	// A needle with its tip at `tip`, pointing along `needle` (unit), is against a body around the vertical line from
	// `top` down to the height `bottomZ`: its tip within `reach` of the line and pointing at it (cosine above
	// `minCosine`). Any units.
	inline bool NeedleAtBody(const Vec3 &tip, const Vec3 &needle, const Vec3 &top, float bottomZ, float reach, float minCosine)
	{
		const Vec3 toLine = ToBodyLine(tip, top, bottomZ);
		const float distance = HandMath::Length(toLine);
		if (distance >= reach)
			return false;
		return distance < 1e-3f || HandMath::Dot(needle, toLine * (1.0f / distance)) > minCosine;
	}

	// Another player's body, from where they stand (their origin, at the feet), game units: the line goes from the knees to
	// the top of the head, lower when they crouch. A hand touches it within kTouchReach meters of the line (the middle of
	// the chest is about 0.15 m in front of it, the outside of the arm 0.25 to the side).
	constexpr float kBodyLineFromFeet = 18.0f;
	constexpr float kBodyLineTopStanding = 66.0f;
	constexpr float kBodyLineTopCrouched = 46.0f;
	constexpr float kTouchReach = 0.32f;

	inline float DistanceToPlayer(const Vec3 &point, const Vec3 &feet, bool crouched)
	{
		const Vec3 top(feet.x, feet.y, feet.z + (crouched ? kBodyLineTopCrouched : kBodyLineTopStanding));
		return DistanceToBodyLine(point, top, feet.z + kBodyLineFromFeet);
	}

	// Of the players whose feet are at `feet` (game units), the one whose body `point` touches, the nearest: its place in
	// the list, -1 if none. `unitsPerMeter` is the game's scale.
	inline int TouchedPlayer(const Vec3 &point, const Vec3 *feet, const bool *crouched, int count, float unitsPerMeter)
	{
		int best = -1;
		float bestDistance = kTouchReach * unitsPerMeter;
		for (int i = 0; i < count; ++i)
		{
			const float distance = DistanceToPlayer(point, feet[i], crouched[i]);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = i;
			}
		}
		return best;
	}
}
