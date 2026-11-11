#pragma once

class Vector;
class QAngle;
struct edict_t;

// A gun in the left hand (left_gun.cpp): it fires with the left trigger, the server shooting its bullets from the left
// hand and taking them from its own magazine. The host needs the mod.
namespace LeftGun
{
	enum FireMode { Semi, Auto, Burst };

	struct GunData
	{
		int weaponId;
		float cycleSeconds;      // between two shots (the scripts' CycleTime, longer for the pump and bolt guns)
		FireMode mode;
		float spread;            // MinStandingSpread of the script
		const char *shotSound;   // the script's single_shot
		bool pistolClick;        // the empty click of a pistol, otherwise a rifle's
		bool twoHanded;          // a long gun, which the right hand can hold too
	};

	// Null for what isn't fired that way (melee weapons, the chainsaw, the grenade launcher)
	const GunData *Find(int weaponId);

	// The server: one shot of the player's left hand, from `origin` towards `angles`
	void ServerFire(edict_t *edict, int playerIndex, const Vector &origin, const QAngle &angles, int seed);
}
