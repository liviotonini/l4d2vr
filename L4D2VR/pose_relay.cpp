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
#include "dual_wield.h"
#include "left_gun.h"

using namespace HandMath;

namespace
{
	const int kFcvarServerCanExecute = 1 << 28;
	const int kMaxHelloAttempts = 8;
	const unsigned long long kHelloIntervalMs = 1500;
	const unsigned long long kKeepAliveIntervalMs = 2000;
	const double kSendIntervalMs = 1000.0 / 30.0;        // the rate the client sends its packets at (cl_cmdrate)
	const unsigned long long kStaleMs = 500;             // the model goes back to the game's animation
	const unsigned long long kClientForgottenMs = 5000;  // a player that stopped uploading doesn't get poses either
	const unsigned long long kNewStreamMs = 1000;        // a pose after this long is taken whatever its clock says
	const double kForwardRate = 40.0;                    // poses per second the server forwards from one player
	const double kForwardBurst = 6.0;                    // how many can come together
	const unsigned long long kReportIntervalMs = 30000;  // how often the log says how the poses of each player arrive

	// GetTickCount64 moves in steps of about 16 ms: the times poses are sent and shown at use the precise counter
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

	double NowMsPrecise() { return NowUs() / 1000.0; }

	// --- The engine's pieces for commands in the unreliable part of the packets (engine.dll of Left 4 Dead 2).
	//
	// CBaseClientState::SendStringCmd and IVEngineServer::ClientCommand build a NET_StringCmd on the stack, set its
	// "reliable" to 1 and give it to the player's channel: SendNetMsg puts a reliable message in the reliable stream
	// whatever its second argument says, so ServerCmd's "reliable" does nothing. The same message with 0 there goes in
	// the next packet.
	const int kStringCmdReliableOffset = 4;
	const int kStringCmdTextOffset = 0x54;
	const int kStringCmdSize = 0x460;       // with the 1 KB buffer it reads into
	const int kSendNetMsgIndex = 41;        // INetChannel::SendNetMsg(INetMessage &, bool forceReliable, bool voice)
	const int kClientNetChannelIndex = 74;  // IVEngineClient::GetNetChannelInfo()
	const int kServerNetChannelIndex = 21;  // IVEngineServer::GetPlayerNetInfo(int playerIndex)

	typedef bool(__thiscall *tSendNetMsg)(void *channel, void *message, bool forceReliable, bool voice);
	typedef void *(__thiscall *tClientNetChannel)(void *engineClient);
	typedef void *(__thiscall *tServerNetChannel)(void *engineServer, int playerIndex);

	bool InEngine(const void *address, size_t size)
	{
		static const HMODULE engine = GetModuleHandleA("engine.dll");
		if (!engine || !address)
			return false;
		const uint8_t *base = (const uint8_t *)engine;
		const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
		const uintptr_t start = (uintptr_t)base, end = start + nt->OptionalHeader.SizeOfImage;
		return (uintptr_t)address >= start && (uintptr_t)address + size <= end;
	}

	// The code starts with these bytes, -1 for any
	bool CodeStartsWith(const void *code, const int *pattern, int length)
	{
		if (!InEngine(code, length))
			return false;
		for (int i = 0; i < length; ++i)
		{
			if (pattern[i] >= 0 && ((const uint8_t *)code)[i] != pattern[i])
				return false;
		}
		return true;
	}

	// The vtable of NET_StringCmd, from IVEngineServer::ClientCommand: "mov byte [ebp+r], 1" sets reliable, and the
	// message starts 4 bytes before, where "mov dword [ebp+r-4], vtable" puts its vtable, with the text 0x54 after it
	void *FindStringCmdVtable(const void *clientCommand)
	{
		if (!InEngine(clientCommand, 0x200))
			return nullptr;

		const uint8_t *code = (const uint8_t *)clientCommand;
		for (int i = 0; i < 0x180; ++i)
		{
			if (code[i] != 0xC6 || code[i + 1] != 0x85 || code[i + 6] != 0x01)
				continue;

			const int32_t message = *(const int32_t *)(code + i + 2) - kStringCmdReliableOffset;
			void *vtable = nullptr;
			bool textThere = false;
			for (int j = 0; j < 0x180; ++j)
			{
				if (code[j] != 0xC7 || code[j + 1] != 0x85)
					continue;
				const int32_t at = *(const int32_t *)(code + j + 2);
				if (at == message)
					vtable = *(void *const *)(code + j + 6);
				else if (at == message + kStringCmdTextOffset)
					textThere = true;
			}

			// Its IsReliable (8th function) reads the byte at +4: mov al, [ecx+4]; ret
			const int isReliable[] = { 0x8A, 0x41, kStringCmdReliableOffset, 0xC3 };
			if (vtable && textThere && InEngine(vtable, 12 * sizeof(void *)) && CodeStartsWith(((void **)vtable)[7], isReliable, 4))
				return vtable;
		}
		return nullptr;
	}

	// SendNetMsg returns with "ret 0Ch" (three arguments) when the channel has no address, and asks the message whether
	// it's reliable (its function at +1Ch)
	bool LooksLikeSendNetMsg(const void *function)
	{
		if (!InEngine(function, 0x40))
			return false;

		const uint8_t *code = (const uint8_t *)function;
		bool returnsThree = false, asksReliable = false;
		for (int i = 0; i < 0x3D; ++i)
		{
			if (code[i] == 0xC2 && code[i + 1] == 0x0C && code[i + 2] == 0x00)
				returnsThree = true;
			if (code[i] == 0x8B && (code[i + 1] & 0xC7) == 0x40 && code[i + 2] == 0x1C)
				asksReliable = true;
		}
		return returnsThree && asksReliable;
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

	// What another player holds in the left hand (dual wield)
	void __cdecl OnLeftItemReceive(const CommandArgs &args)
	{
		if (g_Relay && args.Count() == 3)
			g_Relay->OnLeftItemReceived(atoi(args.Arg(1)), atoi(args.Arg(2)));
	}

	// Testing the dual wield without the headset (VR::DualWieldTest)
	void __cdecl OnDualTest(const CommandArgs &args)
	{
		if (g_Relay && args.Count() == 2)
			g_Relay->OnDualTest(args.Arg(1));
	}

	Command g_AckCommand("l4d2vr_pose_ack", &OnPoseAck, "L4D2VR: the server relays poses.", kFcvarServerCanExecute);
	Command g_ReceiveCommand("l4d2vr_pose_receive", &OnPoseReceive, "L4D2VR: the pose of another player.", kFcvarServerCanExecute);
	Command g_LeftItemCommand("l4d2vr_left_receive", &OnLeftItemReceive, "L4D2VR: what another player holds in the left hand.", kFcvarServerCanExecute);
	Command g_DualTestCommand("l4d2vr_dual_test", &OnDualTest, "L4D2VR: test of the left hand's items (grab, status, swap, use).", 0);

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
	FindUnreliableCommands();

	void *cvar = TryInterface("vstdlib.dll", "VEngineCvar007");
	if (!cvar)
	{
		Diagnostics::Log("pose relay: no console interface, other players' movements are off");
		return;
	}

	((ICvarRegister *)cvar)->RegisterConCommand(&g_AckCommand);
	((ICvarRegister *)cvar)->RegisterConCommand(&g_ReceiveCommand);
	((ICvarRegister *)cvar)->RegisterConCommand(&g_LeftItemCommand);
	((ICvarRegister *)cvar)->RegisterConCommand(&g_DualTestCommand);
}

void PoseRelay::FindUnreliableCommands()
{
	__try
	{
		m_StringCmdVtable = FindStringCmdVtable(m_EngineClientCommand);

		// IVEngineClient::GetNetChannelInfo: call GetBaseLocalClient; mov eax, [eax+10h]; ret
		const int clientGetter[] = { 0xE8, -1, -1, -1, -1, 0x8B, 0x40, 0x10, 0xC3 };
		void *engineClient = m_Game->m_EngineClient;
		void *candidate = engineClient ? (*(void ***)engineClient)[kClientNetChannelIndex] : nullptr;
		if (CodeStartsWith(candidate, clientGetter, 9))
			m_ClientNetChannelGetter = candidate;

		// IVEngineServer::GetPlayerNetInfo: if (index < 1 ...) on its argument first
		const int serverGetter[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xF8, 0x01, 0x7C };
		candidate = m_EngineServer ? (*(void ***)m_EngineServer)[kServerNetChannelIndex] : nullptr;
		if (CodeStartsWith(candidate, serverGetter, 10))
			m_ServerNetChannelGetter = candidate;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		m_StringCmdVtable = nullptr;
	}

	Diagnostics::Log("pose relay: unreliable commands: message %s, client channel %s, server channels %s", m_StringCmdVtable ? "found" : "not found",
		m_ClientNetChannelGetter ? "found" : "not found", m_ServerNetChannelGetter ? "found" : "not found");
}

// Sends a console command to the other side of the channel in the next packet, not in the reliable stream. False if it
// can't, and the caller sends it the usual way.
bool PoseRelay::SendUnreliable(void *netChannel, const char *command)
{
	if (!netChannel || !m_StringCmdVtable || m_UnreliableBroken)
		return false;

	__try
	{
		void *sendNetMsg = (*(void ***)netChannel)[kSendNetMsgIndex];
		if (!m_SendNetMsgChecked)
		{
			m_SendNetMsgChecked = true;
			if (LooksLikeSendNetMsg(sendNetMsg))
				m_SendNetMsg = sendNetMsg;
			else
				Diagnostics::Log("pose relay: INetChannel::SendNetMsg isn't where it was expected, poses go with the reliable commands");
		}
		if (!m_SendNetMsg || sendNetMsg != m_SendNetMsg)
			return false;

		alignas(8) uint8_t message[kStringCmdSize] = {};
		*(void **)message = m_StringCmdVtable;
		message[kStringCmdReliableOffset] = 0;
		*(const char **)(message + kStringCmdTextOffset) = command;
		((tSendNetMsg)m_SendNetMsg)(netChannel, message, false, false);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		m_UnreliableBroken = true;
		Diagnostics::Log("pose relay: sending in the unreliable part of the packets failed, poses go with the reliable commands");
		return false;
	}
}

bool PoseRelay::SendToServerUnreliable(const char *command)
{
	if (!m_ClientNetChannelGetter || !m_Game->m_EngineClient)
		return false;
	return SendUnreliable(((tClientNetChannel)m_ClientNetChannelGetter)(m_Game->m_EngineClient), command);
}

bool PoseRelay::SendToClientUnreliable(int playerIndex, const char *command)
{
	if (!m_ServerNetChannelGetter || !m_EngineServer)
		return false;
	return SendUnreliable(((tServerNetChannel)m_ServerNetChannelGetter)(m_EngineServer, playerIndex), command);
}

void PoseRelay::OnLeftItemReceived(int playerIndex, int itemIndex)
{
	if (playerIndex != m_Game->m_EngineClient->GetLocalPlayer())
		m_Game->m_VR->OnRemoteLeftItem(playerIndex, itemIndex);
}

void PoseRelay::OnDualTest(const char *action)
{
	// remotepose_<player>: that player (a bot) is drawn for 20 s as if it were a VR player sending its arms
	int player = 0;
	if (sscanf_s(action, "remotepose_%d", &player) == 1)
	{
		m_TestPosePlayer = player >= 1 && player <= 32 ? player : 0;
		m_TestPoseUntilMs = NowMs() + 20000;
		Diagnostics::Log("dual test: player %d is drawn with arms from test poses for 20 s", m_TestPosePlayer);
		return;
	}

	m_Game->m_VR->DualWieldTest(action);
}

// The test poses: both hands in front of the player, the right one going round in a small circle and aiming where the
// player looks, as a VR player would send them. They go straight to OnPoseReceived, without the server.
void PoseRelay::UpdateTestPose()
{
	if (m_TestPosePlayer <= 0)
		return;
	if (NowMs() > m_TestPoseUntilMs || m_TestPosePlayer == m_Game->m_EngineClient->GetLocalPlayer())
	{
		Diagnostics::Log("dual test: the test poses of player %d ended", m_TestPosePlayer);
		m_TestPosePlayer = 0;
		return;
	}

	const double now = NowMsPrecise();
	if (now < m_NextTestPoseMs)
		return;
	m_NextTestPoseMs = now + kSendIntervalMs;

	float pitch = 0.0f, yaw = 0.0f;
	if (!m_Game->m_VR->GetPlayerEyeAngles(m_Game->GetClientEntity(m_TestPosePlayer), pitch, yaw))
		yaw = 0.0f;

	Vec3 forward, right;
	YawBasis(yaw, forward, right);
	const Vec3 up(0.0f, 0.0f, 1.0f);
	const float turn = (float)(now / 1000.0 * 3.14159265 * 2.0 * 0.5); // half a turn per second
	const Vec3 rightWrist = forward * 14.0f + right * 6.0f - up * 14.0f + (right * cosf(turn) + up * sinf(turn)) * 4.0f;
	const Vec3 leftWrist = forward * 12.0f - right * 8.0f - up * 16.0f;
	const QAngle level(0.0f, yaw, 0.0f);

	l4d2vr_pose::WirePacket packet;
	packet.versionAndValidMask = (uint8_t)((l4d2vr_pose::kVersion << 4) | l4d2vr_pose::kValidMask);
	packet.featureMask = l4d2vr_pose::kFeatureBodyYaw;
	packet.handStateFlags = l4d2vr_pose::kHandStateRightNativeFingerAnimation | l4d2vr_pose::kHandStateLeftNativeFingerAnimation;
	packet.bodyYaw = PackAngle(yaw);
	PackPoint(Vec3(), level, yaw, packet.hmd);
	PackPoint(rightWrist, level, yaw, packet.rightHand);
	PackPoint(leftWrist, level, yaw, packet.leftHand);
	l4d2vr_pose::FinalizePacket(packet);

	std::string payload;
	if (l4d2vr_pose::EncodePayload(packet, payload))
		OnPoseReceived(m_TestPosePlayer, (uint16_t)(unsigned long long)now, payload.c_str());
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
	{
		player.manualReload = false;
		player.useAimUntil = 0;
	}
	for (Remote &remote : m_Remotes)
	{
		remote.valid = false;
		remote.playout.Reset();
	}
	m_RemoteCount = 0;
	DualWield::ServerReset();
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

	UpdateTestPose();

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
		else if (m_HelloAttempts == kMaxHelloAttempts && now - m_LastHelloMs >= kHelloIntervalMs)
		{
			++m_HelloAttempts;
			Diagnostics::Log("pose relay: the server didn't answer: it doesn't have the mod, or has another version of it (protocol %u here)",
				(unsigned)l4d2vr_pose::kVersion);
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

	// 30 per second, in step with the frames: a pose every 3 frames at 90 per second
	const double nowPrecise = NowMsPrecise();
	if (nowPrecise < m_NextSendMs)
		return;
	m_NextSendMs = nowPrecise - m_NextSendMs > kSendIntervalMs ? nowPrecise + kSendIntervalMs : m_NextSendMs + kSendIntervalMs;

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

	// The sequence is the sender's clock in milliseconds: the receivers show the poses in the rhythm they were made in
	char command[192];
	snprintf(command, sizeof(command), "l4d2vr_pose_upload %u %s", (unsigned)(uint16_t)(unsigned long long)nowPrecise, payload.c_str());
	const bool unreliable = SendToServerUnreliable(command);
	if (!unreliable)
		m_Game->m_EngineClient->ServerCmd(command, false);

	if (!m_LoggedUploadPath)
	{
		m_LoggedUploadPath = true;
		Diagnostics::Log("pose relay: this player's poses go %s", unreliable ? "in the unreliable part of the packets" : "with the reliable commands");
	}
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
		if (args.Count() >= 4)
			ApplyReload(player, atoi(args.Arg(1)), atoi(args.Arg(2)), atoi(args.Arg(3)));
		return true;
	}

	// The hand grabbed something in the world: the use that comes next is aimed from there. With "1" after the angles
	// it was the left hand, which keeps what it takes (dual wield).
	if (strcmp(name, "l4d2vr_use_aim") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && (args.Count() == 7 || args.Count() == 8))
		{
			Player &vrPlayer = m_Game->m_PlayersVRInfo[index];
			vrPlayer.useAimOrigin = Vector((float)atof(args.Arg(1)), (float)atof(args.Arg(2)), (float)atof(args.Arg(3)));
			vrPlayer.useAimAngles = QAngle((float)atof(args.Arg(4)), (float)atof(args.Arg(5)), (float)atof(args.Arg(6)));
			vrPlayer.useAimUntil = NowMs() + 500;
			vrPlayer.useAimLeft = args.Count() == 8 && atoi(args.Arg(7)) != 0;
		}
		return true;
	}

	// What the left hand holds (an entity index, 0 for nothing): the server checks it's the player's, and tells the
	// other players
	if (strcmp(name, "l4d2vr_left") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && (args.Count() == 2 || args.Count() == 3))
		{
			const int item = std::clamp(atoi(args.Arg(1)), 0, 4095);
			const int extra = args.Count() == 3 ? std::clamp(atoi(args.Arg(2)), 0, 4095) : -1;
			DualWield::ServerSetLeftItem(player, index, item, extra);

			char relayed[64];
			snprintf(relayed, sizeof(relayed), "l4d2vr_left_receive %d %d\n", index, item);
			for (int i = 1; i < kMaxPlayers; ++i)
			{
				const ServerClient &recipient = m_Clients[i];
				if (i != index && recipient.introduced && recipient.edict && ServerPlayerIndex(m_Game, recipient.edict) == i)
					SendToClient(recipient.edict, relayed);
			}
		}
		return true;
	}

	// Pills or adrenaline used at once (entity index), or a weapon dropped (entity index and the hand's velocity)
	if (strcmp(name, "l4d2vr_use_item") == 0 || strcmp(name, "l4d2vr_drop") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		const bool use = name[7] == 'u';
		if (m_Game->IsValidPlayerIndex(index) && args.Count() == (use ? 2 : 5))
		{
			const int item = std::clamp(atoi(args.Arg(1)), 0, 4095);
			if (use)
			{
				DualWield::ServerUseItem(player, index, item);
			}
			else
			{
				Vector velocity((float)atof(args.Arg(2)), (float)atof(args.Arg(3)), (float)atof(args.Arg(4)));
				if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z) || VectorLength(velocity) > 1000.0f)
					velocity = Vector(0, 0, 0);
				DualWield::ServerDropItem(player, index, item, velocity);
			}
		}
		return true;
	}

	// A shot of the left hand's gun: x y z pitch yaw roll seed (left_gun.cpp)
	if (strcmp(name, "l4d2vr_left_fire") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && args.Count() == 8)
		{
			const Vector origin((float)atof(args.Arg(1)), (float)atof(args.Arg(2)), (float)atof(args.Arg(3)));
			const QAngle angles((float)atof(args.Arg(4)), (float)atof(args.Arg(5)), (float)atof(args.Arg(6)));
			LeftGun::ServerFire(player, index, origin, angles, atoi(args.Arg(7)));
		}
		return true;
	}

	// The left hand's gun loaded by the right hand, or its ammo taken in the world: the ammo type (left_gun.cpp)
	if (strcmp(name, "l4d2vr_left_reload") == 0 || strcmp(name, "l4d2vr_left_ammo") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && args.Count() == 2)
		{
			if (name[12] == 'r')
				LeftGun::ServerReload(player, index, atoi(args.Arg(1)));
			else
				LeftGun::ServerAmmo(player, index, atoi(args.Arg(1)));
		}
		return true;
	}

	// One weapon entity taken out by the player's hand (dual wield): of two pistols, the one meant
	if (strcmp(name, "l4d2vr_select") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && args.Count() == 2)
			DualWield::ServerSelectWeapon(player, index, std::clamp(atoi(args.Arg(1)), 0, 4095));
		return true;
	}

	// The left hand while it swings a melee weapon: x y z pitch yaw roll flags, and pitch yaw roll where the swing
	// started on its first pose
	if (strcmp(name, "l4d2vr_left_hand") == 0)
	{
		const int index = ServerPlayerIndex(m_Game, player);
		if (m_Game->IsValidPlayerIndex(index) && (args.Count() == 8 || args.Count() == 11))
		{
			const Vector position((float)atof(args.Arg(1)), (float)atof(args.Arg(2)), (float)atof(args.Arg(3)));
			const QAngle angles((float)atof(args.Arg(4)), (float)atof(args.Arg(5)), (float)atof(args.Arg(6)));
			QAngle from = angles;
			if (args.Count() == 11)
				from = QAngle((float)atof(args.Arg(8)), (float)atof(args.Arg(9)), (float)atof(args.Arg(10)));
			const bool finite = std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) && std::isfinite(angles.x)
				&& std::isfinite(angles.y) && std::isfinite(angles.z) && std::isfinite(from.x) && std::isfinite(from.y) && std::isfinite(from.z);
			if (finite)
				DualWield::ServerSetLeftHand(index, position, angles, atoi(args.Arg(7)), &from);
		}
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
			client.forwardTokens = kForwardBurst;
			client.lastRefillUs = NowUs();

			char ack[32];
			snprintf(ack, sizeof(ack), "l4d2vr_pose_ack %u\n", (unsigned)l4d2vr_pose::kVersion);
			SendToClient(player, ack);
		}
		else if (!client.loggedOtherVersion)
		{
			client.loggedOtherVersion = true;
			Diagnostics::Log("pose relay: player %d has another version of the mod (protocol %s, this one %u): no poses to or from them", index,
				args.Arg(1), (unsigned)l4d2vr_pose::kVersion);
		}
		return true;
	}

	if (!client.introduced || args.Count() != 3)
		return true;

	// The sequence is the sender's clock: after a pause it can be anything
	const uint16_t sequence = (uint16_t)atoi(args.Arg(1));
	const bool newStream = nowMs - client.lastUploadMs > kNewStreamMs;
	if (client.haveSequence && !newStream && !l4d2vr_pose::IsSequenceNewer(sequence, client.lastSequence))
		return true;

	// A client can't make the server decode or forward more than this. Poses that come together (in one packet) are
	// all forwarded, so none of the motion is lost.
	const unsigned long long nowUs = NowUs();
	client.forwardTokens = std::min(kForwardBurst, client.forwardTokens + (nowUs - client.lastRefillUs) * (kForwardRate / 1000000.0));
	client.lastRefillUs = nowUs;
	if (client.forwardTokens < 1.0)
		return true;
	client.forwardTokens -= 1.0;

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

		const bool unreliable = SendToClientUnreliable(i, relayed);
		if (!unreliable)
			SendToClient(recipient.edict, relayed);

		if (!m_LoggedForwardPath)
		{
			m_LoggedForwardPath = true;
			Diagnostics::Log("pose relay: the server forwards poses %s", unreliable ? "in the unreliable part of the packets" : "with the reliable commands");
		}
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

	// Out of order or twice: dropped
	if (!remote.playout.Add(sequence, NowMsPrecise(), frame))
		return;

	const unsigned long long now = NowMs();
	if (!remote.valid)
	{
		++m_RemoteCount;
		remote.receivedSinceReport = 0;
		remote.reportedMs = now;
		Diagnostics::Log("pose relay: receiving the poses of player %d", playerIndex);
	}

	remote.valid = true;
	remote.receivedMs = now;
	++remote.receivedSinceReport;

	if (now - remote.reportedMs >= kReportIntervalMs)
	{
		Diagnostics::Log("pose relay: player %d: %.1f poses per second, shown %.0f ms behind the newest, %.0f ms of jitter", playerIndex,
			remote.receivedSinceReport * 1000.0 / (double)(now - remote.reportedMs), remote.playout.DelayBehindNewestMs(), remote.playout.JitterMs());
		remote.receivedSinceReport = 0;
		remote.reportedMs = now;
	}
}

namespace
{
	PoseRelay::Frame BlendFrames(const PoseRelay::Frame &a, const PoseRelay::Frame &b, float t)
	{
		// What isn't a position (which hands it has, whether the left one is on the weapon) from the nearest one
		PoseRelay::Frame out = t < 0.5f ? a : b;
		out.head = BlendPoint(a.head, b.head, t);
		if (a.hasLeftHand && b.hasLeftHand)
			out.leftWrist = BlendPoint(a.leftWrist, b.leftWrist, t);
		if (a.hasRightHand && b.hasRightHand)
			out.rightWrist = BlendPoint(a.rightWrist, b.rightWrist, t);
		if (a.hasLeftCurls && b.hasLeftCurls)
		{
			for (int finger = 0; finger < HandRig::FingerCount; ++finger)
				out.leftCurls[finger] = a.leftCurls[finger] + (b.leftCurls[finger] - a.leftCurls[finger]) * t;
		}
		return out;
	}
}

bool PoseRelay::GetPose(int playerIndex, Frame &out)
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers)
		return false;

	Remote &remote = m_Remotes[playerIndex];
	if (!remote.valid)
		return false;

	if (NowMs() - remote.receivedMs > kStaleMs)
	{
		remote.valid = false;
		remote.playout.Reset();
		--m_RemoteCount;
		return false;
	}

	const Frame *a = nullptr, *b = nullptr;
	float t = 0.0f;
	if (!remote.playout.Sample(NowMsPrecise(), a, b, t))
		return false;

	out = BlendFrames(*a, *b, t);
	return true;
}

void PoseRelay::SetWeaponHold(int playerIndex, const WeaponHold &hold)
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers)
		return;

	m_Remotes[playerIndex].weaponHold = hold;
	m_Remotes[playerIndex].weaponHoldMs = NowMs();
}

// The hand isn't moved (no right hand in the pose): the weapon stays where the game's animation has it, like the hand
void PoseRelay::ClearWeaponHold(int playerIndex)
{
	if (playerIndex >= 1 && playerIndex < kMaxPlayers)
		m_Remotes[playerIndex].weaponHoldMs = 0;
}

bool PoseRelay::GetWeaponHold(int playerIndex, WeaponHold &hold) const
{
	if (playerIndex < 1 || playerIndex >= kMaxPlayers)
		return false;

	const Remote &remote = m_Remotes[playerIndex];
	if (!remote.valid || NowMs() - remote.weaponHoldMs > kStaleMs)
		return false;

	hold = remote.weaponHold;
	return true;
}
