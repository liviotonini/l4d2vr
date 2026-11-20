#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// How a gun aimed with the controller spreads its shots and kicks. No game or SteamVR types, so it can be tested on its
// own (tests/test_aim.cpp). Angles are degrees.
//
// The game spreads a shot before it's fired: CTerrorGun::FireBullet adds a random pitch and yaw, each up to the weapon's
// current spread (m_flSpread), to the eye angles, and passes them to FireTerrorBullets. The mod fires from the
// controller instead, replacing those angles (Hooks::dServerFireTerrorBullets), so it adds a spread again here.
// m_flSpread follows the player's real speed, whatever the keys or the thumbstick asked (MovementSpread), grows with
// every shot and comes back down; the laser sight shrinks it.
//
// A gun held as it should be gets a tenth of what moving and firing add to m_flSpread: standing still it's as precise as
// the controller (ShotSpread). A long gun (any primary weapon but the SMGs) held with one hand gets all of the game's
// spread, plus as much as running adds (OneHandedSpread), and kicks the aim up with every shot (Kick).
namespace Aim
{
	// The game's weapon IDs (C_WeaponCSBase::WeaponID, checked against it where the game's types are)
	enum WeaponId
	{
		Pistol = 1, Uzi = 2, PumpShotgun = 3, AutoShotgun = 4, Rifle = 5, HuntingRifle = 6, Mac10 = 7, ChromeShotgun = 8,
		Scar = 9, MilitarySniper = 10, Spas = 11, GrenadeLauncher = 21, Ak47 = 26, Magnum = 32, Mp5 = 33, Sg552 = 34,
		Awp = 35, Scout = 36, M60 = 37,
	};

	// A gun's spread and punch, from its script (scripts/weapon_*.txt in the game's VPKs)
	struct GunScript
	{
		int weaponId;
		float minDucking;     // MinDuckingSpread
		float minStanding;    // MinStandingSpread
		float minInAir;       // MinInAirSpread
		float maxMovement;    // MaxMovementSpread
		float verticalPunch;  // VerticalPunch: how much a shot kicks the view up in the game
		bool longGun;         // needs both hands: a primary weapon that isn't an SMG
	};

	inline const GunScript *FindGun(int weaponId)
	{
		static const GunScript kGuns[] =
		{
			{ Pistol, 0.5f, 1.5f, 3.0f, 3.0f, 2.0f, false },
			{ Magnum, 0.5f, 1.25f, 3.0f, 3.0f, 4.0f, false },
			{ Uzi, 0.7f, 1.0f, 1.7f, 3.0f, 0.9f, false },
			{ Mac10, 0.85f, 1.2f, 1.7f, 3.0f, 1.0f, false },
			{ Mp5, 0.75f, 1.1f, 1.7f, 3.0f, 1.0f, false },
			{ PumpShotgun, 0.0f, 0.8f, 2.5f, 1.5f, 4.0f, true },
			{ ChromeShotgun, 0.0f, 0.8f, 2.5f, 1.5f, 4.0f, true },
			{ AutoShotgun, 0.0f, 0.8f, 2.5f, 1.5f, 3.0f, true },
			{ Spas, 0.0f, 0.75f, 2.5f, 1.5f, 3.0f, true },
			{ Rifle, 0.05f, 0.4f, 1.5f, 5.0f, 1.2f, true },
			{ Scar, 0.05f, 0.35f, 1.25f, 4.0f, 1.0f, true },
			{ Ak47, 0.5f, 1.0f, 3.0f, 6.0f, 2.0f, true },
			{ Sg552, 0.05f, 0.4f, 1.5f, 5.0f, 1.2f, true },
			{ M60, 0.5f, 1.0f, 3.0f, 6.0f, 2.0f, true },
			{ HuntingRifle, 0.0f, 0.1f, 1.5f, 3.0f, 1.5f, true },
			{ MilitarySniper, 0.05f, 0.5f, 1.5f, 5.0f, 1.5f, true },
			{ Awp, 0.0f, 0.1f, 1.5f, 3.0f, 1.5f, true },
			{ Scout, 0.0f, 0.1f, 1.5f, 3.0f, 1.5f, true },
			{ GrenadeLauncher, 0.05f, 0.4f, 1.5f, 5.0f, 1.2f, true },
		};
		for (const GunScript &gun : kGuns)
		{
			if (gun.weaponId == weaponId)
				return &gun;
		}
		return nullptr;
	}

	inline bool NeedsBothHands(int weaponId)
	{
		const GunScript *gun = FindGun(weaponId);
		return gun && gun->longGun;
	}

	// The game's speed for its spread: at this speed or more a gun spreads the most it does on the ground
	constexpr float kSpreadTopSpeed = 220.0f;
	// The share of the way from the least spread to MaxMovementSpread at that speed, standing and crouched
	constexpr float kStandingMovementShare = 0.75f;
	constexpr float kDuckingMovementShare = 0.5f;

	// What the gun's spread goes to (CTerrorGun's spread update, server.dll 0x3E8EB0): from the least spread standing
	// still (crouched, standing) towards MaxMovementSpread as the real speed grows, in a straight line, up to 220. In the
	// air it's MaxMovementSpread. Without being incapacitated or the laser sight, which the game adds.
	inline float MovementSpread(const GunScript &gun, float speed, bool ducking, bool onGround)
	{
		if (!onGround)
			return gun.maxMovement;
		const float least = ducking ? gun.minDucking : gun.minStanding;
		const float share = std::clamp(speed, 0.0f, kSpreadTopSpeed) / kSpreadTopSpeed * (ducking ? kDuckingMovementShare : kStandingMovementShare);
		return least + (gun.maxMovement - least) * share;
	}

	// A long gun held with one hand: what running at full speed adds to it, on top of whatever it spreads
	inline float OneHandedSpread(const GunScript &gun, float spread)
	{
		return spread + std::max(gun.maxMovement - gun.minStanding, 0.0f) * kStandingMovementShare;
	}

	// A gun held as it should be (a long gun with both hands, a short one with one or two) keeps this much of what the
	// game's spread has above the least standing still: none standing still, a little moving, jumping or firing on
	constexpr float kSteadySpreadShare = 0.1f;

	// What a VR player's shot spreads by, from the gun's own spread (negative if it isn't known) with the game's on.
	// Held as it should be, a tenth of what moving and firing add to it (kSteadySpreadShare). A long gun held with one
	// hand: all of it, and more (OneHandedSpread).
	inline float ShotSpread(const GunScript &gun, float gunSpread, bool gameSpread, bool oneHanded)
	{
		const float spread = gameSpread && gunSpread > 0.0f ? gunSpread : 0.0f;
		if (oneHanded && gun.longGun)
			return OneHandedSpread(gun, spread);
		return std::max(spread - gun.minStanding, 0.0f) * kSteadySpreadShare;
	}

	inline uint32_t Hash(uint32_t x)
	{
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		x ^= x >> 16;
		return x;
	}

	// From -1 to 1
	inline float Signed(uint32_t hash)
	{
		return (float)(hash >> 8) * (2.0f / 16777216.0f) - 1.0f;
	}

	// The random part of a shot, each angle up to `spread` either way, as the game's. The same seed gives the same
	// shot: the client shows it where the server fires it (the seed of the usercmd, or of the left hand's shot).
	inline void SpreadAngles(int seed, float spread, float &pitch, float &yaw)
	{
		const uint32_t first = Hash((uint32_t)seed * 2u + 1u);
		const uint32_t second = Hash(first ^ 0x9E3779B9u);
		pitch = Signed(first) * spread;
		yaw = Signed(second) * spread;
	}

	// Each shot of a long gun held with one hand throws the aim up, by the script's punch, and a little to a side; it
	// comes back by itself. The aim is where the gun points and the shots go.
	constexpr float kKickPerPunch = 2.5f;       // degrees up per unit of VerticalPunch (a rifle 3, a pump shotgun 10)
	constexpr float kKickSideShare = 0.35f;     // up to this much of it to a side
	constexpr float kKickRecoverySeconds = 0.15f;
	constexpr float kKickMost = 20.0f;

	struct Kick
	{
		float pitch = 0.0f;  // up
		float yaw = 0.0f;    // to the left

		void Shot(const GunScript &gun, int seed)
		{
			const float up = gun.verticalPunch * kKickPerPunch;
			pitch = std::min(pitch + up, kKickMost);
			yaw = std::clamp(yaw + Signed(Hash((uint32_t)seed ^ 0x51ED27u)) * up * kKickSideShare, -kKickMost, kKickMost);
		}

		void Update(float seconds)
		{
			const float keep = expf(-std::max(seconds, 0.0f) / kKickRecoverySeconds);
			pitch *= keep;
			yaw *= keep;
			if (fabsf(pitch) < 0.01f && fabsf(yaw) < 0.01f)
				pitch = yaw = 0.0f;
		}

		bool Active() const { return pitch != 0.0f || yaw != 0.0f; }
	};
}
