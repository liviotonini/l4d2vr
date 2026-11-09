#include "pose_relay.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "sdk.h"
#include "sdk_server.h"
#include "game.h"
#include "vr.h"
#include "offsets.h"
#include "vr_pose_protocol.h"
#include "diagnostics.h"

using namespace HandMath;

namespace
{
	const int kFcvarServerCanExecute = 1 << 28;
	const int kMaxHelloAttempts = 8;
	const unsigned long long kHelloIntervalMs = 1500;
	const unsigned long long kKeepAliveIntervalMs = 2000;
	const unsigned long long kSendIntervalMs = 33;       // about 30 per second
	const unsigned long long kStaleMs = 500;             // the model goes back to the game's animation
	const unsigned long long kClientForgottenMs = 5000;  // a player that stopped uploading doesn't get poses either
	const unsigned long long kMinDecodeIntervalUs = 16667;

	unsigned long long NowMs() { return GetTickCount64(); }

	unsigned long long NowUs()
	{
		static const double s_MicrosecondsPerTick = []
		{
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			return 1000000.0 / (double)frequency.QuadPart;
		}();

		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		return (unsigned long long)(counter.QuadPart * s_MicrosecondsPerTick);
	}

	// --- Console commands. Same layout as the engine's classes.
	class CommandArgs
	{
	public:
		int Count() const { return m_Count >= 0 && m_Count <= 64 ? m_Count : 0; }
		const char *Arg(int index) const
		{
			if (index < 0 || index >= Count())
				return "";
			return m_Args[index] ? m_Args[index] : "";
		}

	private:
		int m_Count = 0;
		int m_Argv0Size = 0;
		char m_ArgSBuffer[512]{};
		char m_ArgvBuffer[512]{};
		const char *m_Args[64]{};
	};

	class CommandBase
	{
	public:
		virtual ~CommandBase() = default;
		virtual bool IsCommand() const { return true; }
		virtual bool IsFlagSet(int flag) const { return (m_Flags & flag) != 0; }
		virtual void AddFlags(int flags) { m_Flags |= flags; }
		virtual void RemoveFlags(int flags) { m_Flags &= ~flags; }
		virtual int GetFlags() const { return m_Flags; }
		virtual const char *GetName() const { return m_Name; }
		virtual const char *GetHelpText() const { return m_Help ? m_Help : ""; }
		virtual bool IsRegistered() const { return m_Registered; }
		virtual int GetDLLIdentifier() const { return 0; }

	protected:
		virtual void Create(const char *name, const char *help = nullptr, int flags = 0)
		{
			m_Name = name;
			m_Help = help;
			m_Flags = flags;
		}
		virtual void Init() {}

	public:
		CommandBase *m_Next = nullptr;
		bool m_Registered = false;
		char m_Padding[3] = {};
		const char *m_Name = nullptr;
		const char *m_Help = nullptr;
		int m_Flags = 0;
	};

	class Command : public CommandBase
	{
	public:
		typedef void(__cdecl *Callback)(const CommandArgs &);

		Command(const char *name, Callback callback, const char *help, int flags) : m_Callback(callback)
		{
			Create(name, help, flags);
		}

		bool IsCommand() const override { return true; }
		virtual int AutoCompleteSuggest(const char *, void *) { return 0; }
		virtual bool CanAutoComplete() { return false; }
		virtual void Dispatch(const CommandArgs &args)
		{
			if (m_Callback)
				m_Callback(args);
		}

	private:
		Callback m_Callback = nullptr;
		void *m_CompletionCallback = nullptr;
		bool m_HasCompletionCallback = false;
		bool m_UsingNewCommandCallback = true;
		bool m_UsingCommandCallbackInterface = false;
	};

	class ICvarRegister
	{
	public:
		virtual bool Connect(void *factory) = 0;
		virtual void Disconnect() = 0;
		virtual void *QueryInterface(const char *name) = 0;
		virtual int Init() = 0;
		virtual void Shutdown() = 0;
		virtual int AllocateDLLIdentifier() = 0;
		virtual void RegisterConCommand(void *command) = 0;
	};

	// IVEngineServer::ClientCommand sends a command to the client's console. It takes variable arguments, so `this` is on
	// the stack. (IServerPluginHelpers::ClientCommand has the same name but runs the command on the server, as if the
	// client had sent it, so the client never sees it.)
	typedef void(__cdecl *tEngineClientCommand)(void *thisptr, edict_t *edict, const char *format, ...);

	// Finds it in the interface by the error message only that function has, so a game update doesn't break it
	tEngineClientCommand FindEngineClientCommand(void *engineServer)
	{
		HMODULE engine = GetModuleHandleA("engine.dll");
		if (!engine || !engineServer)
			return nullptr;

		const uint8_t *base = (const uint8_t *)engine;
		const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
		const uintptr_t start = (uintptr_t)base, end = start + nt->OptionalHeader.SizeOfImage;
		const auto inEngine = [&](uintptr_t address, size_t size) { return address >= start && address + size <= end; };

		const char kMessage[] = "StuffCmd:  Some entity tried to stuff";

		void **vtable = *(void ***)engineServer;
		for (int i = 0; i < 200 && inEngine((uintptr_t)vtable[i], 1); ++i)
		{
			const uint8_t *code = (const uint8_t *)vtable[i];
			if (!inEngine((uintptr_t)code, 0x200))
				continue;

			for (int offset = 0; offset < 0x1F0; ++offset)
			{
				if (code[offset] != 0x68) // push <address>
					continue;

				const uintptr_t text = *(const uint32_t *)(code + offset + 1);
				if (inEngine(text, 64) && strstr((const char *)text, kMessage) && strlen((const char *)text) < 200)
				{
					Diagnostics::Log("pose relay: IVEngineServer::ClientCommand is at index %d", i);
					return (tEngineClientCommand)vtable[i];
				}
			}
		}

		return nullptr;
	}

	PoseRelay *g_Relay = nullptr;

	void __cdecl OnPoseAck(const CommandArgs &args)
	{
		if (g_Relay && args.Count() == 2 && atoi(args.Arg(1)) == l4d2vr_pose::kVersion)
			g_Relay->OnAcknowledged();
	}

	void __cdecl OnPoseReceive(const CommandArgs &args)
	{
		if (g_Relay && args.Count() == 4)
			g_Relay->OnPoseReceived(atoi(args.Arg(1)), (uint16_t)atoi(args.Arg(2)), args.Arg(3));
	}

	Command g_AckCommand("l4d2vr_pose_ack", &OnPoseAck, "L4D2VR: the server relays poses.", kFcvarServerCanExecute);
	Command g_ReceiveCommand("l4d2vr_pose_receive", &OnPoseReceive, "L4D2VR: the pose of another player.", kFcvarServerCanExecute);

	void *TryInterface(const char *module, const char *name)
	{
		HMODULE handle = GetModuleHandleA(module);
		if (!handle)
			return nullptr;

		typedef void *(__cdecl *tCreateInterface)(const char *, int *);
		tCreateInterface create = (tCreateInterface)GetProcAddress(handle, "CreateInterface");
		int returnCode = 0;
		return create ? create(name, &returnCode) : nullptr;
	}

	// --- Packing
	float NormalizeAngle(float degrees)
	{
		return degrees - 360.0f * std::floor((degrees + 180.0f) / 360.0f);
	}

	int16_t PackPosition(float value)
	{
		if (!std::isfinite(value))
			return 0;
		return (int16_t)std::lround(std::clamp(value / l4d2vr_pose::kPositionUnitsPerStep, -32767.0f, 32767.0f));
	}

	float UnpackPosition(int16_t value) { return value * l4d2vr_pose::kPositionUnitsPerStep; }

	int16_t PackAngle(float degrees)
	{
		return (int16_t)std::lround(std::clamp(NormalizeAngle(degrees) * (32767.0f / 180.0f), -32767.0f, 32767.0f));
	}

	float UnpackAngle(int16_t value) { return value * (180.0f / 32767.0f); }

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }

	void YawBasis(float yawDegrees, Vec3 &forward, Vec3 &right)
	{
		const float radians = yawDegrees * 3.14159265f / 180.0f;
		forward = Vec3(cosf(radians), sinf(radians), 0.0f);
		right = Vec3(forward.y, -forward.x, 0.0f);
	}

	void PackPoint(const Vec3 &worldOffset, const QAngle &angles, float bodyYaw, l4d2vr_pose::PackedTrackedPose &out)
	{
		Vec3 forward, right;
		YawBasis(bodyYaw, forward, right);
		out.position[0] = PackPosition(Dot(worldOffset, forward));
		out.position[1] = PackPosition(Dot(worldOffset, right));
		out.position[2] = PackPosition(worldOffset.z);
		out.rotation[0] = PackAngle(angles.x);
		out.rotation[1] = PackAngle(angles.y - bodyYaw);
		out.rotation[2] = PackAngle(angles.z);
	}

	PoseRelay::TrackedPoint UnpackPoint(const l4d2vr_pose::PackedTrackedPose &in, float bodyYaw)
	{
		Vec3 forward, right;
		YawBasis(bodyYaw, forward, right);

		PoseRelay::TrackedPoint point;
		const float f = UnpackPosition(in.position[0]), r = UnpackPosition(in.position[1]);
		point.position = forward * f + right * r + Vec3(0.0f, 0.0f, UnpackPosition(in.position[2]));

		const QAngle angles(UnpackAngle(in.rotation[0]), NormalizeAngle(UnpackAngle(in.rotation[1]) + bodyYaw), UnpackAngle(in.rotation[2]));
		Vector vForward, vRight, vUp;
		QAngle::AngleVectors(angles, &vForward, &vRight, &vUp);
		point.forward = ToVec3(vForward);
		point.up = ToVec3(vUp);
		return point;
	}

	PoseRelay::TrackedPoint BlendPoint(const PoseRelay::TrackedPoint &a, const PoseRelay::TrackedPoint &b, float t)
	{
		PoseRelay::TrackedPoint r;
		r.position = Lerp(a.position, b.position, t);
		r.forward = Normalized(Lerp(a.forward, b.forward, t), b.forward);
		r.up = Normalized(Lerp(a.up, b.up, t), b.up);
		return r;
	}

	int ServerPlayerIndex(Game *game, edict_t *player)
	{
		if (!player || !player->m_pUnk)
			return -1;

		typedef int(__thiscall *tEntindex)(void *thisptr);
		static tEntindex entindex = (tEntindex)(game->m_Offsets->CBaseEntity_entindex.address);
		void *entity = player->m_pUnk->GetBaseEntity();
		return entity ? entindex(entity) : -1;
	}
}

void PoseRelay::Init(Game *game)
{
	m_Game = game;
	g_Relay = this;

	m_EngineServer = TryInterface("engine.dll", "VEngineServer022");
	m_EngineClientCommand = (void *)FindEngineClientCommand(m_EngineServer);
	if (!m_EngineClientCommand)
		Diagnostics::Log("pose relay: IVEngineServer::ClientCommand not found, this game won't forward poses when hosting");

	void *cvar = TryInterface("vstdlib.dll", "VEngineCvar007");
	if (!cvar)
	{
		Diagnostics::Log("pose relay: no console interface, other players' movements are off");
		return;
	}

	((ICvarRegister *)cvar)->RegisterConCommand(&g_AckCommand);
	((ICvarRegister *)cvar)->RegisterConCommand(&g_ReceiveCommand);
}

void PoseRelay::Reset()
{
	m_ServerCapable = false;
	m_HelloAttempts = 0;
	m_LastHelloMs = 0;
	m_SentManualReload = -1;
	for (ServerClient &client : m_Clients)
		client = ServerClient();
	for (Player &player : m_Game->m_PlayersVRInfo)
		player.manualReload = false;
	for (Remote &remote : m_Remotes)
		remote.valid = false;
	m_RemoteCount = 0;
}

void PoseRelay::OnAcknowledged()
{
	if (!m_ServerCapable)
		Diagnostics::Log("pose relay: the server forwards poses");
	m_ServerCapable = true;
}

void PoseRelay::Update(VR *vr, C_BasePlayer *localPlayer)
{
	if (!localPlayer)
		return;

	const unsigned long long now = NowMs();

	if (!m_ServerCapable)
	{
		if (m_HelloAttempts < kMaxHelloAttempts && now - m_LastHelloMs >= kHelloIntervalMs)
		{
			m_LastHelloMs = now;
			++m_HelloAttempts;
			char hello[32];
			snprintf(hello, sizeof(hello), "l4d2vr_pose_hello %u", (unsigned)l4d2vr_pose::kVersion);
			m_Game->m_EngineClient->ServerCmd(hello, true);
		}
		return;
	}

	// The server has to know this player reloads by hand, to not reload the weapon by itself
	const int manualReload = vr->m_ManualReloadEnabled ? 1 : 0;
	if (manualReload != m_SentManualReload)
	{
		m_SentManualReload = manualReload;
		char command[40];
		snprintf(command, sizeof(command), "l4d2vr_manual_reload %d", manualReload);
		m_Game->m_EngineClient->ServerCmd(command, true);
	}

	if (!vr->m_ShareBodyMovement && !vr->m_ShowOtherPlayersMovement)
		return;

	// Only receiving: the server forgets players it hasn't heard from
	if (!vr->m_ShareBodyMovement)
	{
		if (now - m_LastHelloMs >= kKeepAliveIntervalMs)
		{
			m_LastHelloMs = now;
			char hello[32];
			snprintf(hello, sizeof(hello), "l4d2vr_pose_hello %u", (unsigned)l4d2vr_pose::kVersion);
			m_Game->m_EngineClient->ServerCmd(hello, true);
		}
		return;
	}

	if (now - m_LastSendMs < kSendIntervalMs)
		return;
	m_LastSendMs = now;

	// Positions are relative to the camera's origin. The receiver only uses the hands relative to the head, so any point
	// that moves with the player works, and this one doesn't need to be read from the entity.
	const Vec3 origin = ToVec3(vr->m_SetupOrigin);
	const float bodyYaw = vr->m_HmdAngAbs.y;

	l4d2vr_pose::WirePacket packet;
	packet.versionAndValidMask = (uint8_t)((l4d2vr_pose::kVersion << 4) | l4d2vr_pose::kValidHmd);
	packet.featureMask = l4d2vr_pose::kFeatureBodyYaw;
	packet.handStateFlags = l4d2vr_pose::kHandStateRightNativeFingerAnimation;
	packet.bodyYaw = PackAngle(bodyYaw);

	PackPoint(ToVec3(vr->m_HmdPosAbs) - origin, vr->m_HmdAngAbs, bodyYaw, packet.hmd);

	if (vr->m_RightControllerDeviceIndex != vr::k_unTrackedDeviceIndexInvalid && vr->m_Poses[vr->m_RightControllerDeviceIndex].bPoseIsValid)
	{
		packet.versionAndValidMask |= l4d2vr_pose::kValidRightHand;
		PackPoint(vr->GetRightWristAbs() - origin, vr->GetRightControllerAbsAngle(), bodyYaw, packet.rightHand);
	}

	if (vr->m_LeftControllerDeviceIndex != vr::k_unTrackedDeviceIndexInvalid && vr->m_Poses[vr->m_LeftControllerDeviceIndex].bPoseIsValid)
	{
		packet.versionAndValidMask |= l4d2vr_pose::kValidLeftHand;

		QAngle leftAngles;
		const Vec3 &f = vr->m_LeftHandForward, &u = vr->m_LeftHandUp;
		QAngle::VectorAngles(Vector(f.x, f.y, f.z), Vector(u.x, u.y, u.z), leftAngles);
		PackPoint(vr->m_LeftHandWrist - origin, leftAngles, bodyYaw, packet.leftHand);

		if (vr->m_LeftHandFollow < 0.5f)
		{
			packet.handStateFlags |= l4d2vr_pose::kHandStateLeftNativeFingerAnimation;
			if (vr->isHoldingWeapon2Handed)
				packet.handStateFlags |= l4d2vr_pose::kHandStateTwoHandedGrip;
		}
		else
		{
			packet.featureMask |= l4d2vr_pose::kFeatureLeftFingerCurls;
			for (int finger = 0; finger < HandRig::FingerCount; ++finger)
				packet.leftFingerCurls[finger] = (uint8_t)std::lround(std::clamp(vr->m_LeftFingerCurls[finger] / l4d2vr_pose::kFingerCurlMaximum, 0.0f, 1.0f) * 255.0f);
		}
	}

	l4d2vr_pose::FinalizePacket(packet);

	std::string payload;
	if (!l4d2vr_pose::EncodePayload(packet, payload))
		return;

	char command[192];
	snprintf(command, sizeof(command), "l4d2vr_pose_upload %u %s", (unsigned)++m_Sequence, payload.c_str());
	m_Game->m_EngineClient->ServerCmd(command, false);
}

bool PoseRelay::SendToClient(edict_t *edict, const char *command)
{
	if (!m_EngineClientCommand || !edict)
		return false;

	__try
	{
		((tEngineClientCommand)m_EngineClientCommand)(m_EngineServer, edict, "%s", command);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

bool PoseRelay::HandleServerCommand(edict_t *player, const void *commandObject)
{
	const CommandArgs &args = *(const CommandArgs *)commandObject;
	const char *name = args.Arg(0);

	if (strcmp(name, "l4d2vr_reload") == 0)
	{
		if (args.Count() == 4)
			ApplyReload(player, atoi(args.Arg(1)), atoi(args.Arg(2)), atoi(args.Arg(3)));
		return true;
	}

	if (strcmp(name, "l4d2vr_manual_reload") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && args.Count() == 2)
			m_Game->m_PlayersVRInfo[index].manualReload = atoi(args.Arg(1)) != 0;
		return true;
	}

	const bool isHello = strcmp(name, "l4d2vr_pose_hello") == 0;
	const bool isUpload = strcmp(name, "l4d2vr_pose_upload") == 0;
	if (!isHello && !isUpload)
		return false;

	const int index = ServerPlayerIndex(m_Game, player);
	if (index < 1 || index >= kMaxPlayers)
		return true;

	ServerClient &client = m_Clients[index];
	if (client.edict != player)
	{
		client = ServerClient();
		client.edict = player;
	}

	const unsigned long long nowMs = NowMs();

	if (isHello)
	{
		if (args.Count() == 2 && atoi(args.Arg(1)) == l4d2vr_pose::kVersion)
		{
			client.introduced = true;
			client.haveSequence = false;
			client.lastUploadMs = nowMs;

			char ack[32];
			snprintf(ack, sizeof(ack), "l4d2vr_pose_ack %u\n", (unsigned)l4d2vr_pose::kVersion);
			SendToClient(player, ack);
		}
		return true;
	}

	if (!client.introduced || args.Count() != 3)
		return true;

	const uint16_t sequence = (uint16_t)atoi(args.Arg(1));
	if (client.haveSequence && !l4d2vr_pose::IsSequenceNewer(sequence, client.lastSequence))
		return true;

	// A client can't make the server decode or forward more than this
	const unsigned long long nowUs = NowUs();
	if (client.lastDecodeUs != 0 && nowUs - client.lastDecodeUs < kMinDecodeIntervalUs)
		return true;
	client.lastDecodeUs = nowUs;

	l4d2vr_pose::WirePacket packet;
	const char *payload = args.Arg(2);
	if (!l4d2vr_pose::DecodePayload(payload, packet))
		return true;

	client.haveSequence = true;
	client.lastSequence = sequence;
	client.lastUploadMs = nowMs;

	char relayed[192];
	const int length = snprintf(relayed, sizeof(relayed), "l4d2vr_pose_receive %d %u %s\n", index, (unsigned)sequence, payload);
	if (length <= 0 || length >= (int)sizeof(relayed))
		return true;

	for (int i = 1; i < kMaxPlayers; ++i)
	{
		ServerClient &recipient = m_Clients[i];
		if (i == index || !recipient.introduced || !recipient.edict)
			continue;

		// Gone, or a different player took the slot
		if (nowMs - recipient.lastUploadMs > kClientForgottenMs || ServerPlayerIndex(m_Game, recipient.edict) != i)
		{
			recipient = ServerClient();
			continue;
		}

		SendToClient(recipient.edict, relayed);
	}

	return true;
}

// On the server: finishes a reload done with the hands, for the weapon the player is holding. The ammo is worked out
// here, the client only says what it did.
void PoseRelay::ApplyReload(edict_t *player, int shell, int ammoType, int expectedReserve)
{
	const int kClipOffset = 0x1414;
	const int kInReloadOffset = 0x144D;
	const int kPartialReloadOffset = 0x17D8;
	const int kReloadFromEmptyOffset = 0x17DF;
	const int kAmmoArrayOffset = 0x1874;
	const int kMaxReserveDifference = 80; // the client's number can be a little behind

	if (!player || !player->m_pUnk)
		return;

	void *serverPlayer = player->m_pUnk->GetBaseEntity();
	if (!serverPlayer)
		return;

	typedef Server_WeaponCSBase *(__thiscall *tGetActiveWeapon)(void *thisptr);
	tGetActiveWeapon getActiveWeapon = (tGetActiveWeapon)(m_Game->m_Offsets->GetActiveWeapon.address);
	Server_WeaponCSBase *weapon = getActiveWeapon ? getActiveWeapon(serverPlayer) : nullptr;
	if (!weapon)
		return;

	const int weaponId = weapon->GetWeaponID();
	const int capacity = VR::GetReloadCapacity(weaponId);
	if (capacity <= 0)
		return;

	int &clip = *(int *)((uint8_t *)weapon + kClipOffset);

	if (VR::ReloadHasUnlimitedReserve(weaponId))
	{
		if (clip >= 0 && clip <= 1000)
			clip = std::max(capacity, clip);
		*((uint8_t *)weapon + kInReloadOffset) = 0;
		return;
	}

	if (ammoType < 0 || ammoType >= 32)
		return;

	int &reserve = *(int *)((uint8_t *)serverPlayer + kAmmoArrayOffset + ammoType * 4);

	// If this isn't the number the client has, these offsets are wrong for this version of the game: nothing is written
	if (clip < 0 || clip > 1000 || reserve < 0 || abs(reserve - expectedReserve) > kMaxReserveDifference)
	{
		Diagnostics::Log("manual reload: unexpected values on the server (clip %d, reserve %d, client's reserve %d), ignored", clip, reserve, expectedReserve);
		return;
	}

	if (shell)
	{
		if (clip < capacity && reserve > 0)
		{
			++clip;
			--reserve;
		}
	}
	else
	{
		// What was left in the old magazine isn't lost, like in the game's reload
		const int total = clip + reserve;
		const int newClip = std::min(std::max(capacity, clip), total);
		reserve = total - newClip;
		clip = newClip;
	}

	// If the game had started its own reload
	*((uint8_t *)weapon + kInReloadOffset) = 0;
	*((uint8_t *)weapon + kPartialReloadOffset) = 0;
	*((uint8_t *)weapon + kReloadFromEmptyOffset) = 0;
}

void PoseRelay::OnPoseReceived(int playerIndex, uint16_t sequence, const char *payload)
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers || !payload || playerIndex == m_Game->m_EngineClient->GetLocalPlayer())
		return;

	l4d2vr_pose::WirePacket packet;
	if (!l4d2vr_pose::DecodePayload(payload, packet))
		return;

	Remote &remote = m_Remotes[playerIndex];
	if (remote.valid && !l4d2vr_pose::IsSequenceNewer(sequence, remote.sequence) && NowMs() - remote.receivedMs < 2000)
		return;

	const uint8_t validMask = l4d2vr_pose::PacketValidMask(packet);
	const uint8_t features = l4d2vr_pose::PacketFeatureMask(packet);
	const uint8_t handState = l4d2vr_pose::PacketHandStateFlags(packet);
	if (!(validMask & l4d2vr_pose::kValidHmd))
		return;

	const float bodyYaw = UnpackAngle(packet.bodyYaw);

	Frame frame;
	frame.head = UnpackPoint(packet.hmd, bodyYaw);
	frame.leftWrist = UnpackPoint(packet.leftHand, bodyYaw);
	frame.rightWrist = UnpackPoint(packet.rightHand, bodyYaw);
	frame.hasLeftHand = (validMask & l4d2vr_pose::kValidLeftHand) != 0;
	frame.hasRightHand = (validMask & l4d2vr_pose::kValidRightHand) != 0;
	frame.leftOnWeapon = (handState & l4d2vr_pose::kHandStateTwoHandedGrip) != 0;
	frame.hasLeftCurls = (features & l4d2vr_pose::kFeatureLeftFingerCurls) != 0;
	for (int finger = 0; finger < HandRig::FingerCount; ++finger)
		frame.leftCurls[finger] = packet.leftFingerCurls[finger] * (l4d2vr_pose::kFingerCurlMaximum / 255.0f);

	const unsigned long long now = NowMs();
	if (remote.valid)
	{
		remote.previous = remote.latest;
		remote.havePrevious = true;
		remote.intervalMs = std::clamp((float)(now - remote.receivedMs), 10.0f, 120.0f);
	}
	else
	{
		remote.havePrevious = false;
		++m_RemoteCount;
	}

	remote.valid = true;
	remote.sequence = sequence;
	remote.latest = frame;
	remote.receivedMs = now;
}

bool PoseRelay::GetPose(int playerIndex, Frame &out)
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers)
		return false;

	Remote &remote = m_Remotes[playerIndex];
	if (!remote.valid)
		return false;

	const unsigned long long now = NowMs();
	if (now - remote.receivedMs > kStaleMs)
	{
		remote.valid = false;
		--m_RemoteCount;
		return false;
	}

	if (!remote.havePrevious)
	{
		out = remote.latest;
		return true;
	}

	// Shown one packet late, moving smoothly from the one before to the last one
	const float t = std::clamp((float)(now - remote.receivedMs) / remote.intervalMs, 0.0f, 1.0f);
	out = remote.latest;
	out.head = BlendPoint(remote.previous.head, remote.latest.head, t);
	out.leftWrist = BlendPoint(remote.previous.leftWrist, remote.latest.leftWrist, t);
	out.rightWrist = BlendPoint(remote.previous.rightWrist, remote.latest.rightWrist, t);
	for (int finger = 0; finger < HandRig::FingerCount; ++finger)
		out.leftCurls[finger] = remote.previous.leftCurls[finger] + (remote.latest.leftCurls[finger] - remote.previous.leftCurls[finger]) * t;
	return true;
}

void PoseRelay::SetWeaponDelta(int playerIndex, const Mat34 &delta)
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers)
		return;

	m_Remotes[playerIndex].weaponDelta = delta;
	m_Remotes[playerIndex].weaponDeltaMs = NowMs();
}

bool PoseRelay::GetWeaponDelta(int playerIndex, Mat34 &delta) const
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers)
		return false;

	const Remote &remote = m_Remotes[playerIndex];
	if (!remote.valid || NowMs() - remote.weaponDeltaMs > kStaleMs)
		return false;

	delta = remote.weaponDelta;
	return true;
}
