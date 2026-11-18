#pragma once
#include "hand_math.h"

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
		const char *shotSound;   // the script's single_shot
		bool pistolClick;        // the empty click of a pistol, otherwise a rifle's
		bool twoHanded;          // a long gun, which the right hand can hold too
		bool shells;             // loaded a shell at a time
		int reserveMax;          // the most ammo carried for it (the game's ammo_*_max), 0 for pistols and the M60
		const char *loadSound;   // its magazine put in, null if none
		const char *muzzleFlash; // the script's MuzzleFlashEffect_3rdPerson
		const char *brass;       // the script's EjectBrassEffect
		float recoilDegrees;     // how much a shot throws its muzzle up in the hand
		bool workedByHand;       // a pump or bolt action: nothing moves by itself when it fires
	};

	// Null for what isn't fired that way (melee weapons, the chainsaw, the grenade launcher)
	const GunData *Find(int weaponId);

	// Where the parts of a gun's world model are, from its weapon bone (x to its left, y up the grip, z along the barrel),
	// game units. Measured from the models' vertices: the bones of most of them are elsewhere (the magazine's 10 units
	// ahead, the bolt's at the grip), or have nothing on them.
	struct GunModel
	{
		const char *model;          // the file's name, without the folder
		HandMath::Vec3 magazine;    // the middle of the magazine, or where shells are loaded
		float magazineHalf;         // from there to the end of the magazine that sticks out, down the grip
		HandMath::Vec3 bolt;        // where the slide, the charging handle, the bolt or the pump is taken
		bool pumpIsClip;            // the pump is the model's "weapon_bone_Clip" (pump shotguns): it moves when pulled
		HandMath::Vec3 grip;        // the middle of the pistol grip, or of the stock's wrist, where the fist closes
		float gripRake;             // how much the grip leans back at its bottom, degrees from the weapon bone's y
	};

	// Null for a model that isn't one of the game's guns
	const GunModel *FindModel(const char *modelName);

	// The server: one shot of the player's left hand, from `origin` towards `angles`
	void ServerFire(edict_t *edict, int playerIndex, const Vector &origin, const QAngle &angles, int seed);
	// The right hand put a magazine (or a shell) in the left hand's gun, from the ammo of `ammoType`
	void ServerReload(edict_t *edict, int playerIndex, int ammoType);
	// The right hand took ammo in the world: the left hand's gun's ammo is full too
	void ServerAmmo(edict_t *edict, int playerIndex, int ammoType);
}
