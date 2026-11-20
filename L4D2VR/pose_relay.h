#pragma once
#include <cstdint>
#include "hand_math.h"
#include "hand_rig.h"
#include "pose_playout.h"

class Game;
class VR;
class C_BasePlayer;
struct edict_t;

// Sends the VR player's head and hands to the other players who have the mod, and receives theirs.
//
// Each player uploads a small packet 30 times per second with a console command to the server. The server (a listen
// server of a player with the mod) forwards it to the others, who use it when drawing that player's model. Everyone
// involved needs the mod, including the host. The cost is a few KB per second and a few microseconds per VR player
// drawn; nothing happens for players who don't send poses.
//
// The engine sends console commands in the reliable stream, which has one batch in flight at a time: what's sent
// meanwhile waits for it to be acknowledged, so poses arrived in bursts once per round trip. Poses go in the unreliable
// part of the packets instead (the same message, marked unreliable), and a lost one is replaced by the next. Each pose
// carries the sender's clock and is shown in the rhythm it was made in (PosePlayout).
class PoseRelay
{
public:
	static constexpr int kMaxPlayers = 65;

	struct TrackedPoint
	{
		HandMath::Vec3 position; // relative to the player's origin
		HandMath::Vec3 forward;
		HandMath::Vec3 up;
	};

	struct Frame
	{
		TrackedPoint head;
		TrackedPoint leftWrist;
		TrackedPoint rightWrist;
		float leftCurls[HandRig::FingerCount] = {};
		bool hasLeftHand = false;
		bool hasRightHand = false;
		bool hasLeftCurls = false;
		bool leftOnWeapon = false;
	};

	void Init(Game *game);

	// Client: says hello to the server and uploads the local player's pose
	void Update(VR *vr, C_BasePlayer *localPlayer);

	// Server: true if the command was one of ours
	bool HandleServerCommand(edict_t *player, const void *command);

	// A map change starts over, every player has to introduce itself again
	void Reset();

	// The pose of a player to show now: a little behind, interpolated in the rhythm it was sent in. False if the player
	// isn't sending.
	bool GetPose(int playerIndex, Frame &out);

	bool HasRemotePlayers() const { return m_RemoteCount > 0; }
	bool IsServerCapable() const { return m_ServerCapable; }

	// How the weapon in a player's right hand follows the hand: set when that player is drawn, used when the weapon is
	struct WeaponHold
	{
		HandMath::Mat34 delta;          // from the hand of the game's animation to where the mod put it
		HandMath::Mat34 animatedBone;   // "weapon_bone" in the game's animation, which the weapon is merged to
		bool hasAnimatedBone = false;
	};
	void SetWeaponHold(int playerIndex, const WeaponHold &hold);
	void ClearWeaponHold(int playerIndex);
	bool GetWeaponHold(int playerIndex, WeaponHold &hold) const;

	// Received from the server through console commands
	void OnAcknowledged();
	void OnPoseReceived(int playerIndex, uint16_t sequence, const char *payload);
	void OnLeftItemReceived(int playerIndex, int itemIndex);
	void OnDualTest(const char *action);

private:
	struct ServerClient
	{
		edict_t *edict = nullptr;
		bool introduced = false;
		bool haveSequence = false;
		bool loggedOtherVersion = false;
		uint16_t lastSequence = 0;
		unsigned long long lastUploadMs = 0;
		double forwardTokens = 0.0;          // how many poses it can still have forwarded right now
		unsigned long long lastRefillUs = 0;
	};

	struct Remote
	{
		bool valid = false;
		PosePlayout::Buffer<Frame> playout;
		unsigned long long receivedMs = 0;
		WeaponHold weaponHold;
		unsigned long long weaponHoldMs = 0;
		int receivedSinceReport = 0;
		unsigned long long reportedMs = 0;
	};

	Game *m_Game = nullptr;
	void *m_EngineServer = nullptr;
	void *m_EngineClientCommand = nullptr;

	bool m_ServerCapable = false;
	int m_HelloAttempts = 0;
	unsigned long long m_LastHelloMs = 0;
	double m_NextSendMs = 0.0;
	int m_SentManualReload = -1;

	ServerClient m_Clients[kMaxPlayers];
	Remote m_Remotes[kMaxPlayers];
	int m_RemoteCount = 0;

	// Commands in the unreliable part of the packets: the engine's NET_StringCmd marked unreliable, given to the
	// player's INetChannel::SendNetMsg. Found in the engine's code at start; without them, the reliable commands.
	void *m_StringCmdVtable = nullptr;
	void *m_SendNetMsg = nullptr;             // INetChannel::SendNetMsg, checked on the first channel
	bool m_SendNetMsgChecked = false;
	void *m_ClientNetChannelGetter = nullptr; // IVEngineClient::GetNetChannelInfo
	void *m_ServerNetChannelGetter = nullptr; // IVEngineServer::GetPlayerNetInfo
	bool m_UnreliableBroken = false;
	bool m_LoggedUploadPath = false;
	bool m_LoggedForwardPath = false;

	// l4d2vr_dual_test remotepose_<player>
	int m_TestPosePlayer = 0;
	unsigned long long m_TestPoseUntilMs = 0;
	double m_NextTestPoseMs = 0.0;
	void UpdateTestPose();

	void FindUnreliableCommands();
	bool SendUnreliable(void *netChannel, const char *command);
	bool SendToServerUnreliable(const char *command);
	bool SendToClientUnreliable(int playerIndex, const char *command);
	bool SendToClient(edict_t *edict, const char *command);
	void ApplyReload(edict_t *player, int shell, int ammoType, int expectedReserve);
	void ApplyEject(edict_t *player, int weaponId);
};
