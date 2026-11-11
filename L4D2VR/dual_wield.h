#pragma once

class Game;
class Vector;
class QAngle;
struct edict_t;

// Dual wield: the left hand holds a melee weapon, a grenade, pills or adrenaline while the right hand holds the weapon
// (dual_wield.cpp).
//
// The game has one active weapon: the right hand's. The left hand's item stays in the inventory, drawn in the hand by
// the mod. Its melee hits are traced by the server from the left hand; grenades, pills and adrenaline become the
// active weapon only while they're used. The host needs the mod.
//
// A pistol and a melee weapon share a slot, so taking one drops the other. A melee weapon taken with the left hand is
// given a slot the game doesn't use instead (GetSlot), on the server and on the client, and keeps it while the player
// has it.
namespace DualWield
{
	constexpr int kExtraSlot = 6;
	constexpr int kMaxPlayers = 65;

	bool IsLeftHandItem(int weaponId);
	// The class name to select a weapon with "use" (empty if the mod doesn't know it)
	const char *ClassName(int weaponId);

	// --- Server
	void ServerInit(Game *game);
	void ServerReset();
	// CBaseCombatWeapon::GetSlot: true with the slot to use instead of the script's
	bool ServerGetSlot(void *weapon, int &slot);
	// CBasePlayer::PlayerUse: a weapon taken with the left hand gets the extra slot
	void ServerBeginUse(void *player, int playerIndex);
	void ServerEndUse(void *player, int playerIndex);
	// After the usercmds of a player: checks what the left hand holds, and traces its melee swing
	void ServerAfterUsercmds(edict_t *edict, void *player, int playerIndex);
	// Commands from the player (PoseRelay::HandleServerCommand)
	void ServerSetLeftItem(edict_t *edict, int playerIndex, int itemIndex);
	void ServerSetLeftHand(int playerIndex, const Vector &position, const QAngle &angles, int flags);

	// Flags of l4d2vr_left_hand
	constexpr int kLeftHandSwinging = 1;
	constexpr int kLeftHandUsing = 2;

	// --- Client
	// The local player's melee weapon in the extra slot (entity index, 0 if none), kept by VR::UpdateDualWield
	void ClientSetExtraMelee(int entityIndex);
	bool ClientGetSlot(void *weapon, int &slot);
}
