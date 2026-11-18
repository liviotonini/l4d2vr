#pragma once

class Game;
class Vector;
class QAngle;
struct edict_t;

// Dual wield: the left hand holds any weapon or item while the right hand holds its own (dual_wield.cpp).
//
// The game has one active weapon: the right hand's. The left hand's item stays in the inventory, drawn in the hand by
// the mod. Its melee hits are traced by the server from the left hand; grenades, kits and the like become the active
// weapon only while they're used, pills and adrenaline are used at the mouth or the body. The host needs the mod.
//
// The game carries one weapon per slot, so taking one drops the other of its slot. A weapon of the primary or secondary
// slot in the left hand is given a slot the game doesn't use instead (GetSlot), on the server and on the client: the
// extra slot, the left shoulder's. It keeps it while the player has it, unless the right hand takes it to a slot of its
// own that is free. Two pistols stay two weapons for VR players: the game doesn't make a pair of them.
namespace DualWield
{
	constexpr int kExtraSlot = 6;
	constexpr int kMaxPlayers = 65;

	// Weapons and items the left hand can hold (the ones carried in the arms, like the gas can, aside)
	bool IsLeftHandItem(int weaponId);
	// Weapons of the primary and secondary slots: in the extra slot while the left hand has them
	bool UsesExtraSlot(int weaponId);
	// Items the left trigger uses by taking them out for a moment: grenades, kits, the defibrillator, upgrade packs
	bool IsLeftUseItem(int weaponId);

	// --- Server
	void ServerInit(Game *game);
	void ServerReset();
	// CBaseCombatWeapon::GetSlot: true with the slot to use instead of the script's
	bool ServerGetSlot(void *weapon, int &slot);
	// CBasePlayer::PlayerUse: a weapon taken with the left hand gets the extra slot
	void ServerBeginUse(void *player, int playerIndex);
	void ServerEndUse(void *player, int playerIndex);
	// During that use: true if `weapon`, theirs, is the other hand's (the left hand's is the one in the extra slot, in it
	// or behind the shoulder)
	bool ServerOtherHandHolds(void *player, void *weapon);
	// After the usercmds of a player: checks what the left hand holds, and traces its melee swing
	void ServerAfterUsercmds(edict_t *edict, void *player, int playerIndex);
	// Commands from the player (PoseRelay::HandleServerCommand). `extraIndex` is the weapon in the extra slot, -1 if the
	// player didn't say.
	void ServerSetLeftItem(edict_t *edict, int playerIndex, int itemIndex, int extraIndex);
	// The player takes out one weapon entity (where "use <class>" takes the first one of two of the same kind)
	void ServerSelectWeapon(edict_t *edict, int playerIndex, int weaponIndex);
	// `from`: where the swing started, with kLeftHandNewSwing
	void ServerSetLeftHand(int playerIndex, const Vector &position, const QAngle &angles, int flags, const QAngle *from);
	// Pills or adrenaline used at once, without the game's animation, from either hand
	void ServerUseItem(edict_t *edict, int playerIndex, int itemIndex);
	// A weapon or item dropped from a hand
	void ServerDropItem(edict_t *edict, int playerIndex, int weaponIndex, const Vector &velocity);
	// Pills or adrenaline given from either hand to the player it touches, the way the game gives them
	void ServerGiveItem(edict_t *edict, int playerIndex, int itemIndex, int targetIndex);
	// The player the kit touches, to heal (l4d2vr_heal_target), 0 for nobody
	void ServerSetHealTarget(int playerIndex, int targetIndex);
	// The player a VR player's kit heals with its secondary attack (Hooks::dServerBackpackTarget): the one it touches if
	// they're near, null for nobody
	void *ServerHealTarget(void *player, int playerIndex);
	// The player's weapon is taken out. False if it failed.
	bool ServerTakeOut(void *player, void *weapon);
	// The weapon `index` of `player`, null if the player doesn't have it
	void *ServerPlayersWeapon(void *player, int index);
	// The server's entity `index`, null if none
	void *ServerEntity(int index);
	// The server's time (gpGlobals->curtime), 0 if it isn't known
	float ServerCurtime();
	// The gun a laser sight goes on (Hooks::dPlayerAddUpgrade): the left hand's for a VR player who took it with that
	// hand, otherwise the gun out (pistols too). Null for the game's own choice, the primary weapon.
	void *ServerUpgradeWeapon(void *player);
	// A field of the entity written by the mod goes to the players (CBaseEntity::NetworkStateChanged)
	void ServerNetworkChanged(void *entity);

	// Flags of l4d2vr_left_hand
	constexpr int kLeftHandSwinging = 1;
	constexpr int kLeftHandUsing = 2;
	// The first pose of a swing: the hand was slower than the swing speed just before, like a usercmd of the right
	// hand without the swing flag. Comes with the angles of the hand from just before.
	constexpr int kLeftHandNewSwing = 4;

	// --- Client
	// The local player's weapon in the extra slot (entity index, 0 if none), kept by VR::UpdateDualWield
	void ClientSetExtraWeapon(int entityIndex);
	bool ClientGetSlot(void *weapon, int &slot);
}
