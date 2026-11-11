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

	// The pistol's grip on the thigh of the weapon hand, standing: height above the floor and distance to the side
	constexpr float kHolsterHeight = 0.475f;
	constexpr float kHolsterSide = 0.1535f;

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

	// Where the pills (0) and the grenade (1) are when config.txt doesn't say: by the shoulders. Meters from the eyes.
	struct ItemSpot
	{
		float forward, side, below;
	};

	inline ItemSpot DefaultItemSpot(int spot, float height)
	{
		return spot == 0 ? ItemSpot{ 0.11f, -0.13f, 0.2056f * height } : ItemSpot{ 0.10f, 0.16f, 0.1889f * height };
	}

	inline Vec3 ItemSpotOffset(const ItemSpot &spot, bool leftHanded)
	{
		return Vec3(spot.forward, (leftHanded ? 1.0f : -1.0f) * spot.side, -spot.below);
	}
}
