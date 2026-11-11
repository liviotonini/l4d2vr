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

	// A needle with its tip at `tip`, pointing along `needle` (unit), is against a body around the vertical line from
	// `top` down to the height `bottomZ`: its tip within `reach` of the line and pointing at it (cosine above
	// `minCosine`). Any units.
	inline bool NeedleAtBody(const Vec3 &tip, const Vec3 &needle, const Vec3 &top, float bottomZ, float reach, float minCosine)
	{
		const float down = std::max(top.z - bottomZ, 0.0f);
		const float along = std::clamp(top.z - tip.z, 0.0f, down);
		const Vec3 toLine = Vec3(top.x, top.y, top.z - along) - tip;
		const float distance = HandMath::Length(toLine);
		if (distance >= reach)
			return false;
		return distance < 1e-3f || HandMath::Dot(needle, toLine * (1.0f / distance)) > minCosine;
	}
}
