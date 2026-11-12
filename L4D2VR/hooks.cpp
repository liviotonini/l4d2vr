#include "hooks.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "vr.h"
#include "offsets.h"
#include "diagnostics.h"
#include "studio.h"
#include "pose_relay.h"
#include "dual_wield.h"
#include <algorithm>
#include <iostream>

// Interacting with the hand: while the game looks for what the player wants to use, the player's eyes are moved to the
// hand, so it finds what the hand points at. The game then does everything else as usual.
// The server decides what gets used, so the host needs the mod. The client only decides what is highlighted.
namespace
{
	struct UseAim
	{
		void *player = nullptr;
		Vector origin = { 0,0,0 };
		QAngle angles = { 0,0,0 };
	};

	UseAim g_ServerUseAim;
	UseAim g_ClientUseAim;
}

Hooks::Hooks(Game *game)
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
	}

	m_Game = game;
	m_VR = m_Game->m_VR;

	m_PushHUDStep = -999;
	m_PushedHud = false;

	initSourceHooks();

	hkGetRenderTarget.enableHook();
	hkCalcViewModelView.enableHook();
	hkServerFireTerrorBullets.enableHook();
	hkClientFireTerrorBullets.enableHook();
	hkProcessUsercmds.enableHook();
	hkReadUsercmd.enableHook();
	hkWriteUsercmdDeltaToBuffer.enableHook();
	hkWriteUsercmd.enableHook();
	hkAdjustEngineViewport.enableHook();
	hkViewport.enableHook();
	hkGetViewport.enableHook();
	hkCreateMove.enableHook();
	hkTestMeleeSwingCollisionClient.enableHook();
	hkTestMeleeSwingCollisionServer.enableHook();
	hkDoMeleeSwingServer.enableHook();
	hkStartMeleeSwingServer.enableHook();
	hkPrimaryAttackServer.enableHook();
	hkItemPostFrameServer.enableHook();
	hkGetPrimaryAttackActivity.enableHook();
	hkEyePosition.enableHook();
	hkDrawModelExecute.enableHook();
	hkRenderView.enableHook();
	hkPushRenderTargetAndViewport.enableHook();
	hkPopRenderTargetAndViewport.enableHook();
	hkVgui_Paint.enableHook();
	hkIsSplitScreen.enableHook();
	hkPrePushRenderTarget.enableHook();
	hkFormatViewModelAttachment.enableHook();
	if (hkStartGrenadeThrow.pTarget)
		hkStartGrenadeThrow.enableHook();
	for (auto *hook : { &hkFindUseEntity, &hkClientFindUseEntity })
		if (hook->pTarget)
			hook->enableHook();
	if (hkPlayerUse.pTarget)
		hkPlayerUse.enableHook();
	if (hkServerPlayerEyePosition.pTarget)
	{
		hkServerPlayerEyePosition.enableHook();
		hkServerPlayerEyeAngles.enableHook();
	}
	if (hkClientPlayerEyePosition.pTarget)
	{
		hkClientPlayerEyePosition.enableHook();
		hkClientPlayerEyeVectors.enableHook();
	}
	if (hkServerClientCommand.pTarget)
		hkServerClientCommand.enableHook();
	if (hkServerDefaultReload.pTarget)
		hkServerDefaultReload.enableHook();
	if (hkClientDefaultReload.pTarget)
		hkClientDefaultReload.enableHook();
	if (hkServerShotgunReload.pTarget)
		hkServerShotgunReload.enableHook();
	if (hkClientShotgunReload.pTarget)
		hkClientShotgunReload.enableHook();
	if (hkServerSetHelpingHandState.pTarget)
		hkServerSetHelpingHandState.enableHook();
	if (hkClientSetHelpingHandState.pTarget)
		hkClientSetHelpingHandState.enableHook();
	if (hkClientGetSlot.pTarget)
		hkClientGetSlot.enableHook();
	if (hkServerGetSlot.pTarget)
		hkServerGetSlot.enableHook();
	if (hkServerAddDualWeapon.pTarget)
		hkServerAddDualWeapon.enableHook();
	if (hkServerWeaponOfId.pTarget)
		hkServerWeaponOfId.enableHook();
	if (hkServerWeaponById.pTarget)
		hkServerWeaponById.enableHook();
	if (hkClientParticleTracer.pTarget)
		hkClientParticleTracer.enableHook();
	if (hkGrenadeLauncherCreate.pTarget)
		hkGrenadeLauncherCreate.enableHook();
}

Hooks::~Hooks()
{
	if (MH_Uninitialize() != MH_OK)
	{
		Game::errorMsg("Failed to uninitialize MinHook");
	}
}


int Hooks::initSourceHooks()
{
	LPVOID pGetRenderTargetVFunc = (LPVOID)(m_Game->m_Offsets->GetRenderTarget.address);
	hkGetRenderTarget.createHook(pGetRenderTargetVFunc, &dGetRenderTarget);

	LPVOID pRenderViewVFunc = (LPVOID)(m_Game->m_Offsets->RenderView.address);
	hkRenderView.createHook(pRenderViewVFunc, &dRenderView);

	LPVOID calcViewModelViewAddr = (LPVOID)(m_Game->m_Offsets->CalcViewModelView.address);
	hkCalcViewModelView.createHook(calcViewModelViewAddr, &dCalcViewModelView);

	LPVOID serverFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ServerFireTerrorBullets.address);
	hkServerFireTerrorBullets.createHook(serverFireTerrorBulletsAddr, &dServerFireTerrorBullets);

	LPVOID clientFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ClientFireTerrorBullets.address);
	hkClientFireTerrorBullets.createHook(clientFireTerrorBulletsAddr, &dClientFireTerrorBullets);

	LPVOID ProcessUsercmdsAddr = (LPVOID)(m_Game->m_Offsets->ProcessUsercmds.address);
	hkProcessUsercmds.createHook(ProcessUsercmdsAddr, &dProcessUsercmds);

	LPVOID ReadUserCmdAddr = (LPVOID)(m_Game->m_Offsets->ReadUserCmd.address);
	hkReadUsercmd.createHook(ReadUserCmdAddr, &dReadUsercmd);

	LPVOID WriteUsercmdDeltaToBufferAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmdDeltaToBuffer.address);
	hkWriteUsercmdDeltaToBuffer.createHook(WriteUsercmdDeltaToBufferAddr, &dWriteUsercmdDeltaToBuffer);

	LPVOID WriteUsercmdAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmd.address);
	hkWriteUsercmd.createHook(WriteUsercmdAddr, &dWriteUsercmd);

	LPVOID AdjustEngineViewportAddr = (LPVOID)(m_Game->m_Offsets->AdjustEngineViewport.address);
	hkAdjustEngineViewport.createHook(AdjustEngineViewportAddr, &dAdjustEngineViewport);

	LPVOID ViewportAddr = (LPVOID)(m_Game->m_Offsets->Viewport.address);
	hkViewport.createHook(ViewportAddr, &dViewport);

	LPVOID GetViewportAddr = (LPVOID)(m_Game->m_Offsets->GetViewport.address);
	hkGetViewport.createHook(GetViewportAddr, &dGetViewport);

	LPVOID MeleeSwingClientAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingClient.address);
	hkTestMeleeSwingCollisionClient.createHook(MeleeSwingClientAddr, &dTestMeleeSwingCollisionClient);

	LPVOID MeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingServer.address);
	hkTestMeleeSwingCollisionServer.createHook(MeleeSwingServerAddr, &dTestMeleeSwingCollisionServer);

	LPVOID DoMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->DoMeleeSwingServer.address);
	hkDoMeleeSwingServer.createHook(DoMeleeSwingServerAddr, &dDoMeleeSwingServer);

	LPVOID StartMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->StartMeleeSwingServer.address);
	hkStartMeleeSwingServer.createHook(StartMeleeSwingServerAddr, &dStartMeleeSwingServer);

	LPVOID PrimaryAttackServerAddr = (LPVOID)(m_Game->m_Offsets->PrimaryAttackServer.address);
	hkPrimaryAttackServer.createHook(PrimaryAttackServerAddr, &dPrimaryAttackServer);

	LPVOID ItemPostFrameServerAddr = (LPVOID)(m_Game->m_Offsets->ItemPostFrameServer.address);
	hkItemPostFrameServer.createHook(ItemPostFrameServerAddr, &dItemPostFrameServer);

	LPVOID GetPrimaryAttackActivityAddr = (LPVOID)(m_Game->m_Offsets->GetPrimaryAttackActivity.address);
	hkGetPrimaryAttackActivity.createHook(GetPrimaryAttackActivityAddr, &dGetPrimaryAttackActivity);

	LPVOID EyePositionAddr = (LPVOID)(m_Game->m_Offsets->EyePosition.address);
	hkEyePosition.createHook(EyePositionAddr, &dEyePosition);

	LPVOID DrawModelExecuteAddr = (LPVOID)(m_Game->m_Offsets->DrawModelExecute.address);
	hkDrawModelExecute.createHook(DrawModelExecuteAddr, &dDrawModelExecute);

	LPVOID PushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PushRenderTargetAndViewport.address);
	hkPushRenderTargetAndViewport.createHook(PushRenderTargetAddr, &dPushRenderTargetAndViewport);

	LPVOID PopRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PopRenderTargetAndViewport.address);
	hkPopRenderTargetAndViewport.createHook(PopRenderTargetAddr, &dPopRenderTargetAndViewport);

	LPVOID VGui_PaintAddr = (LPVOID)(m_Game->m_Offsets->VGui_Paint.address);
	hkVgui_Paint.createHook(VGui_PaintAddr, &dVGui_Paint);

	LPVOID IsSplitScreenAddr = (LPVOID)(m_Game->m_Offsets->IsSplitScreen.address);
	hkIsSplitScreen.createHook(IsSplitScreenAddr, &dIsSplitScreen);

	LPVOID PrePushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PrePushRenderTarget.address);
	hkPrePushRenderTarget.createHook(PrePushRenderTargetAddr, &dPrePushRenderTarget);

	LPVOID FormatViewModelAttachmentAddr = (LPVOID)(m_Game->m_Offsets->FormatViewModelAttachment.address);
	hkFormatViewModelAttachment.createHook(FormatViewModelAttachmentAddr, &dFormatViewModelAttachment);

	initGrenadeThrowHooks();
	initUseHooks();
	initRelayHook();
	initWeaponStateHooks();

	void *clientMode = nullptr;
	while (!clientMode)
	{
		Sleep(10);
		clientMode = **(void ***)(m_Game->m_Offsets->g_pClientMode.address);
	}
	hkCreateMove.createHook( (*(void ***)clientMode)[27], dCreateMove );

	return 1;
}

// Grenade throw with the arm (VR::UpdateGrenadeThrow).
// EmitGrenade is a virtual function that each grenade class overrides, so it's replaced in their vtables
// (HookEmitGrenade) instead of with MinHook. Its vtable offset and the owner lookup are read from the code of
// CBaseCSGrenade::ThrowGrenade:
//   +0x018  call CBaseCombatWeapon::GetPlayerOwner
//   +0x3EB  mov edx, [edx + EmitGrenade offset]   (regular throw)
// The throws are replaced for the host's own player and for VR players who joined (Player::throw*).
void Hooks::initGrenadeThrowHooks()
{
	const uintptr_t throwGrenade = m_Game->m_Offsets->ThrowGrenade.address;
	const uintptr_t startGrenadeThrow = m_Game->m_Offsets->StartGrenadeThrow.address;
	if (!throwGrenade || !startGrenadeThrow)
		return;

	const uint8_t *code = (const uint8_t *)throwGrenade;
	const int emitGrenadeOffset = *(const int32_t *)(code + 0x3ED);
	if (code[0x18] != 0xE8 || code[0x3EB] != 0x8B || code[0x3EC] != 0x92 || emitGrenadeOffset <= 0 || emitGrenadeOffset > 0x2000 || emitGrenadeOffset % 4 != 0)
	{
		Game::errorMsg("CBaseCSGrenade::ThrowGrenade doesn't look as expected, throwing grenades with the arm is disabled.");
		return;
	}

	// +0x356  movss xmm3, [esi + m_vecAbsVelocity]   (the player's velocity added to the throw)
	const int absVelocityOffset = *(const int32_t *)(code + 0x35A);
	if (code[0x356] != 0xF3 || code[0x357] != 0x0F || code[0x358] != 0x10 || code[0x359] != 0x9E || absVelocityOffset <= 0 || absVelocityOffset > 0x4000)
	{
		Game::errorMsg("CBaseCSGrenade::ThrowGrenade doesn't look as expected, throwing grenades with the arm is disabled.");
		return;
	}

	m_GetGrenadeOwner = (tGetPlayerOwner)(throwGrenade + 0x18 + 5 + *(const int32_t *)(code + 0x19));
	m_EmitGrenadeVtableOffset = emitGrenadeOffset;
	m_AbsVelocityOffset = absVelocityOffset;
	m_ThrowGrenade = (tThrowGrenade)throwGrenade;

	hkStartGrenadeThrow.createHook((LPVOID)startGrenadeThrow, &dStartGrenadeThrow);
}


ITexture *__fastcall Hooks::dGetRenderTarget(void *ecx, void *edx)
{
	ITexture *result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dRenderView(void *ecx, void *edx, CViewSetup &setup, CViewSetup &hudViewSetup, int nClearFlags, int whatToDraw)
{
	if (!m_VR->m_CreatedVRTextures)
		m_VR->CreateVRTextures();

	IMatRenderContext *rndrContext = m_Game->m_MaterialSystem->GetRenderContext();

	// Scope view of the sniper rifles. Rendered before the eyes so the last view rendered in the frame is still an eye.
	if (m_VR->UpdateScope())
	{
		Diagnostics::ScopedTimer scopeTimer(Diagnostics::Section_Scope);
		CViewSetup scopeView = setup;
		scopeView.x = 0;
		scopeView.y = 0;
		scopeView.width = VR::kScopeTextureSize;
		scopeView.height = VR::kScopeTextureSize;
		scopeView.m_nUnscaledX = 0;
		scopeView.m_nUnscaledY = 0;
		scopeView.m_nUnscaledWidth = VR::kScopeTextureSize;
		scopeView.m_nUnscaledHeight = VR::kScopeTextureSize;
		scopeView.fov = m_VR->m_ScopeFov;
		scopeView.fovViewmodel = m_VR->m_ScopeFov;
		scopeView.m_flAspectRatio = 1.0f;
		scopeView.zNear = 6;
		scopeView.zNearViewmodel = 6;
		scopeView.origin = m_VR->m_ScopeCameraPos;
		scopeView.angles = Vector(m_VR->m_ScopeCameraAng.x, m_VR->m_ScopeCameraAng.y, m_VR->m_ScopeCameraAng.z);

		constexpr int kRenderViewDrawViewModel = 1 << 0; // RENDERVIEW_DRAWVIEWMODEL

		m_RenderingScope = true;
		rndrContext->SetRenderTarget(m_VR->m_ScopeTexture);
		hkRenderView.fOriginal(ecx, scopeView, hudViewSetup, nClearFlags, whatToDraw & ~kRenderViewDrawViewModel);
		m_RenderingScope = false;

		m_VR->m_ScopeRendered = true;
	}

	Diagnostics::ScopedTimer eyesTimer(Diagnostics::Section_Eyes);

	CViewSetup leftEyeView = setup;
	CViewSetup rightEyeView = setup;

	// Left eye CViewSetup
	leftEyeView.x = 0;
	leftEyeView.width = m_VR->m_RenderWidth;
	leftEyeView.height = m_VR->m_RenderHeight;
	leftEyeView.fov = m_VR->m_Fov;
	leftEyeView.fovViewmodel = m_VR->m_Fov;
	leftEyeView.m_flAspectRatio = m_VR->m_Aspect;
	leftEyeView.zNear = 6;
	leftEyeView.zNearViewmodel = 6;
	leftEyeView.origin = m_VR->GetViewOriginLeft();
	leftEyeView.angles = m_VR->GetViewAngle();

	m_VR->m_SetupOrigin = setup.origin;

	Vector hmdAngle = m_VR->GetViewAngle();
	QAngle inGameAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z);
	m_Game->m_EngineClient->SetViewAngles(inGameAngle);

	rndrContext->SetRenderTarget(m_VR->m_LeftEyeTexture);
	hkRenderView.fOriginal(ecx, leftEyeView, hudViewSetup, nClearFlags, whatToDraw);
	m_PushedHud = false;

	// Right eye CViewSetup
	rightEyeView.x = 0;
	rightEyeView.width = m_VR->m_RenderWidth;
	rightEyeView.height = m_VR->m_RenderHeight;
	rightEyeView.fov = m_VR->m_Fov;
	rightEyeView.fovViewmodel = m_VR->m_Fov;
	rightEyeView.m_flAspectRatio = m_VR->m_Aspect;
	rightEyeView.zNear = 6;
	rightEyeView.zNearViewmodel = 6;
	rightEyeView.origin = m_VR->GetViewOriginRight();
	rightEyeView.angles = m_VR->GetViewAngle();

	rndrContext->SetRenderTarget(m_VR->m_RightEyeTexture);
	hkRenderView.fOriginal(ecx, rightEyeView, hudViewSetup, nClearFlags, whatToDraw);

	m_VR->m_RenderedNewFrame = true;
}

bool __fastcall Hooks::dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd)
{
	if (!cmd->command_number)
		return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	if (m_VR->m_IsVREnabled && m_VR->m_RoomscaleActive)
	{
		Vector setupOriginToHMD = m_VR->m_SetupOriginToHMD;
		setupOriginToHMD.z = 0;
		float distance = VectorLength(setupOriginToHMD);
		if (distance > 1)
		{
			float forwardSpeed = DotProduct2D(setupOriginToHMD, m_VR->m_HmdForward);
			float sideSpeed = DotProduct2D(setupOriginToHMD, m_VR->m_HmdRight);
			cmd->forwardmove += distance * forwardSpeed;
			cmd->sidemove += distance * sideSpeed;
		}
	}

	return false;
}

void __fastcall Hooks::dEndFrame(void *ecx, void *edx)
{
	return hkEndFrame.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void *ecx, void *edx, void *owner, const Vector &eyePosition, const QAngle &eyeAngles)
{
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	if (m_VR->m_IsVREnabled)
	{
		vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
		vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
	}

	return hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
}

int Hooks::dServerFireTerrorBullets(int playerId, const Vector &vecOrigin, const QAngle &vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// Server host
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		vecNewOrigin = m_VR->GetRightControllerAbsPos();
		vecNewAngles = m_VR->GetRightControllerAbsAngle();
	}
	// Clients
	else if (m_Game->IsValidPlayerIndex(playerId) && m_Game->m_PlayersVRInfo[playerId].isUsingVR)
	{
		vecNewOrigin = m_Game->m_PlayersVRInfo[playerId].controllerPos;
		vecNewAngles = m_Game->m_PlayersVRInfo[playerId].controllerAngle;
	}

	return hkServerFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}

int Hooks::dClientFireTerrorBullets(int playerId, const Vector &vecOrigin, const QAngle &vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;
	
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		vecNewOrigin = m_VR->GetRightControllerAbsPos();
		vecNewAngles = m_VR->GetRightControllerAbsAngle();
	}

	return hkClientFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}


int Hooks::FireServerBullets(int playerIndex, const Vector &origin, const QAngle &angles, int weaponId, int seed, float spread)
{
	__try
	{
		return hkServerFireTerrorBullets.fOriginal(playerIndex, origin, angles, weaponId, 0, seed, spread);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Diagnostics::Log("left gun: the server's shot of player %d failed", playerIndex);
		return 0;
	}
}

// The hits and tracers of the shot here, at once (the server's go to the other players). The game draws the tracers
// from the weapon in the right hand: they leave from `origin` instead (dClientParticleTracer).
int Hooks::FireClientBullets(int playerIndex, const Vector &origin, const QAngle &angles, int weaponId, int seed, float spread)
{
	m_LeftShotStart = &origin;
	int result = 0;
	__try
	{
		result = hkClientFireTerrorBullets.fOriginal(playerIndex, origin, angles, weaponId, 0, seed, spread);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	m_LeftShotStart = nullptr;
	return result;
}

float __fastcall Hooks::dProcessUsercmds(void *ecx, void *edx, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused)
{
	// Function pointer for CBaseEntity::entindex
	typedef int(__thiscall *tEntindex)(void *thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

	IServerUnknown * pUnknown = player ? player->m_pUnk : nullptr;
	Server_BaseEntity *pPlayer = pUnknown ? (Server_BaseEntity*)pUnknown->GetBaseEntity() : nullptr;

	int index = pPlayer ? oEntindex(pPlayer) : -1;
	m_Game->m_CurrentUsercmdID = index;

	float result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);

	if (!m_Game->IsValidPlayerIndex(index))
		return result;

	// check if swinging melee wep
	if (m_Game->m_PlayersVRInfo[index].isUsingVR && m_Game->m_PlayersVRInfo[index].isMeleeing)
	{
		typedef Server_WeaponCSBase *(__thiscall *tGetActiveWep)(void *thisptr);
		static tGetActiveWep oGetActiveWep = (tGetActiveWep)(m_Game->m_Offsets->GetActiveWeapon.address);
		Server_WeaponCSBase *curWep = oGetActiveWep(pPlayer);
		
		if (curWep)
		{
			int wepID = curWep->GetWeaponID();
			if (wepID == 19) // melee weapon
			{
				Player &vrPlayer = m_Game->m_PlayersVRInfo[index];
				TraceMeleeSwing(curWep, vrPlayer.prevControllerAngle, vrPlayer.controllerAngle, nullptr, vrPlayer.isNewSwing);
				vrPlayer.isNewSwing = false;
			}
		}
	}
	else
	{
		m_Game->m_PlayersVRInfo[index].isNewSwing = true;
	}

	m_Game->m_PlayersVRInfo[index].prevControllerAngle = m_Game->m_PlayersVRInfo[index].controllerAngle;

	// What the left hand holds, and its melee hits (dual wield)
	DualWield::ServerAfterUsercmds(player, pPlayer, index);

	return result;
}

// A melee weapon swung from one angle of the hand to another: 10 traces from the hand (dEyePosition), the right one
// when `hand` is null
void Hooks::TraceMeleeSwing(void *weapon, const QAngle &from, const QAngle &to, const Vector *hand, bool newSwing)
{
	Server_WeaponCSBase *curWep = (Server_WeaponCSBase *)weapon;
	if (newSwing)
		curWep->entitiesHitThisSwing = 0;

	typedef void *(__thiscall *tGetMeleeWepInfo)(void *thisptr);
	static tGetMeleeWepInfo oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfo.address);
	void *meleeWepInfo = oGetMeleeWepInfo(curWep);

	Vector initialForward, initialRight, initialUp;
	QAngle::AngleVectors(from, &initialForward, &initialRight, &initialUp);
	Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0);
	VectorNormalize(initialMeleeDirection);

	Vector finalForward, finalRight, finalUp;
	QAngle::AngleVectors(to, &finalForward, &finalRight, &finalUp);
	Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0);
	VectorNormalize(finalMeleeDirection);

	Vector pivot;
	CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
	VectorNormalize(pivot);

	// The same direction twice would be acos of a little more than 1
	const float cosine = std::clamp(DotProduct(initialMeleeDirection, finalMeleeDirection), -1.0f, 1.0f);
	float swingAngle = acos(cosine) * 180 / 3.14159265;

	m_Game->m_Hooks->hkGetPrimaryAttackActivity.fOriginal(curWep, meleeWepInfo); // Needed to call TestMeleeSwingCollision

	m_Game->m_MeleeEyeOverride = hand;
	m_Game->m_PerformingMelee = true;

	Vector traceDirection = initialMeleeDirection;
	int numTraces = 10;
	float traceAngle = swingAngle / numTraces;
	for (int i = 0; i < numTraces; ++i)
	{
		traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
		m_Game->m_Hooks->hkTestMeleeSwingCollisionServer.fOriginal(curWep, traceDirection);
	}

	m_Game->m_PerformingMelee = false;
	m_Game->m_MeleeEyeOverride = nullptr;
}

int Hooks::dReadUsercmd(void *buf, CUserCmd *move, CUserCmd *from)
{
	hkReadUsercmd.fOriginal(buf, move, from);

	int i = m_Game->m_CurrentUsercmdID;
	if (!m_Game->IsValidPlayerIndex(i))
		return 1;

	const int kInReload = 1 << 13;
	m_Game->m_PlayersVRInfo[i].reloadButton = (move->buttons & kInReload) != 0;

	if (move->tick_count < 0) // Signal for VR CUserCmd
	{
		move->tick_count *= -1;

		if (move->command_number < 0)
		{
			move->command_number *= -1;
			m_Game->m_PlayersVRInfo[i].isMeleeing = true;
		}
		else
		{
			m_Game->m_PlayersVRInfo[i].isMeleeing = false;
		}

		m_Game->m_PlayersVRInfo[i].isUsingVR = true;

		bool throwGesture;
		int throwSequence;
		Vector throwArmVelocity;
		if (VR::DecodeGrenadeThrow(move->mousedx, move->mousedy, throwGesture, throwSequence, throwArmVelocity))
		{
			// A grenade throw in place of the controller angles (VR::GetGrenadeThrowMessage), which keep their last value.
			// The same throw comes in several usercmds.
			Player &player = m_Game->m_PlayersVRInfo[i];
			const auto now = std::chrono::steady_clock::now();
			if (throwSequence != player.throwSequence || now - player.throwTime > std::chrono::seconds(3))
			{
				player.throwSequence = throwSequence;
				player.throwPending = true;
				player.throwGesture = throwGesture;
				player.throwArmVelocity = throwArmVelocity;
				player.throwTime = now;
			}
		}
		else
		{
			m_Game->m_PlayersVRInfo[i].controllerAngle.x = (float)move->mousedx / 10;
			m_Game->m_PlayersVRInfo[i].controllerAngle.y = (float)move->mousedy / 10;
		}
		m_Game->m_PlayersVRInfo[i].controllerPos.x = move->viewangles.z;
		m_Game->m_PlayersVRInfo[i].controllerPos.y = move->upmove;

		// Decode controllerAngle.z
		int rollEncoding = move->command_number / 10000000;
		move->command_number -= rollEncoding * 10000000;
		m_Game->m_PlayersVRInfo[i].controllerAngle.z = (rollEncoding * 2) - 180;

		// Decode viewangles.x
		int decodedZInt = (move->viewangles.x / 10000);
		float decodedAngle = abs((float)(move->viewangles.x - (decodedZInt * 10000)) / 10);
		decodedAngle -= 360;
		float decodedZ = (float)decodedZInt / 10;

		m_Game->m_PlayersVRInfo[i].controllerPos.z = decodedZ;

		move->viewangles.x = decodedAngle;
		move->viewangles.z = 0;
		move->upmove = 0;
	}
	else
	{
		m_Game->m_PlayersVRInfo[i].isUsingVR = false;
	}
	static_assert(sizeof(CUserCmd) <= sizeof(Player::lastCmd), "the copy of the usercmd is too small");
	memcpy(m_Game->m_PlayersVRInfo[i].lastCmd, move, sizeof(CUserCmd));
	m_Game->m_PlayersVRInfo[i].hasLastCmd = true;
	return 1;
}

void __fastcall Hooks::dWriteUsercmdDeltaToBuffer(void *ecx, void *edx, int a1, void *buf, int from, int to, bool isnewcommand) 
{
	return hkWriteUsercmdDeltaToBuffer.fOriginal(ecx, a1, buf, from, to, isnewcommand);
}

int Hooks::dWriteUsercmd(void *buf, CUserCmd *to, CUserCmd *from)
{
	if (m_VR->m_IsVREnabled)
	{
		CInput *m_Input = **(CInput ***)(m_Game->m_Offsets->g_pppInput.address);
		CVerifiedUserCmd *pVerifiedCommands = *(CVerifiedUserCmd **)((uintptr_t)m_Input + 0xF0);
		CVerifiedUserCmd *pVerified = &pVerifiedCommands[(to->command_number) % 150];

		// Signal to the server that this CUserCmd has VR info
		to->tick_count *= -1;

		int originalCommandNum = to->command_number;

		QAngle controllerAngles = m_VR->GetRightControllerAbsAngle();
		short throwMousedx, throwMousedy;
		if (m_VR->GetGrenadeThrowMessage(throwMousedx, throwMousedy))
		{
			// Just let go of a grenade: send the throw instead of the angles (no gun to aim while throwing)
			to->mousedx = throwMousedx;
			to->mousedy = throwMousedy;
		}
		else
		{
			to->mousedx = controllerAngles.x * 10; // Strip off 2nd decimal to save bits.
			to->mousedy = controllerAngles.y * 10;
		}
		int rollEncoding = (((int)controllerAngles.z + 180) / 2 * 10000000);
		to->command_number += rollEncoding;

		if (VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > m_VR->m_MeleeSwingVelocityRequired && !m_VR->m_RightHandEmpty)
		{
			to->command_number *= -1; // Signal to server that melee swing in motion
		}

		Vector controllerPos = m_VR->GetRightControllerAbsPos();
		to->viewangles.z = controllerPos.x;
		to->upmove = controllerPos.y;

		// Space in CUserCmd is tight, so encode viewangle.x and controllerPos.z together.
		// Encoding will overflow if controllerPos.z goes beyond +-21474.8
		float xAngle = to->viewangles.x;
		int encodedAngle = (xAngle + 360) * 10;
		int encoding = (int)(controllerPos.z * 10) * 10000;
		encoding += encoding < 0 ? -encodedAngle : encodedAngle;
		to->viewangles.x = encoding;

		hkWriteUsercmd.fOriginal(buf, to, from);

		to->viewangles.x = xAngle;
		to->tick_count *= -1;
		to->viewangles.z = 0;
		to->upmove = 0;
		to->command_number = originalCommandNum;

		// Must recalculate checksum for the edited CUserCmd or gunshots will sound
		// terrible in multiplayer.
		pVerified->m_cmd = *to;
		pVerified->m_crc = to->GetChecksum();
		return 1;
	}
	return hkWriteUsercmd.fOriginal(buf, to, from);
}

void Hooks::dAdjustEngineViewport(int &x, int &y, int &width, int &height)
{
	hkAdjustEngineViewport.fOriginal(x, y, width, height);
}

void Hooks::dViewport(void *ecx, void *edx, int x, int y, int width, int height)
{
	hkViewport.fOriginal(ecx, x, y, width, height);
}

void Hooks::dGetViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height)
{
	hkGetViewport.fOriginal(ecx, x, y, width, height);
}

int Hooks::dTestMeleeSwingCollisionClient(void *ecx, void *edx, Vector const &vec)
{
	return hkTestMeleeSwingCollisionClient.fOriginal(ecx, vec);
}

int Hooks::dTestMeleeSwingCollisionServer(void *ecx, void *edx, Vector const &vec)
{
	return hkTestMeleeSwingCollisionServer.fOriginal(ecx, vec);
}

void Hooks::dDoMeleeSwingServer(void *ecx, void *edx)
{
	return hkDoMeleeSwingServer.fOriginal(ecx);
}

void Hooks::dStartMeleeSwingServer(void *ecx, void *edx, void *player, bool a3)
{
	return hkStartMeleeSwingServer.fOriginal(ecx, player, a3);
}

int Hooks::dPrimaryAttackServer(void *ecx, void *edx)
{
	return hkPrimaryAttackServer.fOriginal(ecx);
}

void Hooks::dItemPostFrameServer(void *ecx, void *edx)
{
	hkItemPostFrameServer.fOriginal(ecx);
}

int Hooks::dGetPrimaryAttackActivity(void *ecx, void *edx, void *meleeInfo)
{
	return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

Vector *Hooks::dEyePosition(void *ecx, void *edx, Vector *eyePos)
{
	if (eyePos && g_ServerUseAim.player == ecx)
	{
		*eyePos = g_ServerUseAim.origin;
		return eyePos;
	}

	Vector *result = hkEyePosition.fOriginal(ecx, eyePos);

	if (m_Game->m_PerformingMelee)
	{
		int i = m_Game->m_CurrentUsercmdID;
		*result = m_Game->m_MeleeEyeOverride ? *m_Game->m_MeleeEyeOverride : m_Game->m_PlayersVRInfo[i].controllerPos;
	}

	return result;
}

// What a model is, by the path of its file. Called for every model drawn, so it's looked up by pointer.
Hooks::ModelKind Hooks::GetModelKind(const void *model)
{
	auto found = m_ModelKinds.find(model);
	if (found != m_ModelKinds.end())
		return found->second;

	const char *modelName = m_Game->m_ModelInfo->GetModelName((void *)model);
	ModelKind kind = ModelKind::Other;
	if (modelName)
	{
		// The game's scripts mix upper and lower case (v_dual_pistolA.mdl)
		char name[260];
		strncpy_s(name, modelName, _TRUNCATE);
		_strlwr_s(name);

		if (strstr(name, "/arms/v_arms"))
			kind = ModelKind::FirstPersonArms;
		else if (strstr(name, "models/v_models/v_"))
			kind = strstr(name, "dual_pistola") ? ModelKind::DualPistolsViewmodel : ModelKind::Viewmodel;
		// Melee weapons in first person are elsewhere, without them what's on the body wasn't drawn
		else if (strstr(name, "models/weapons/melee/v_"))
			kind = ModelKind::Viewmodel;
		else if (strstr(name, "models/survivors/survivor_"))
			kind = ModelKind::Survivor;
		else if (strstr(name, "models/w_models/") || strstr(name, "models/weapons/melee/w_"))
			kind = ModelKind::WorldWeapon;
	}

	m_ModelKinds[model] = kind;
	return kind;
}

void Hooks::dDrawModelExecute(void *ecx, void *edx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
	const ModelKind kind = (m_VR->m_IsVREnabled && info.pModel && state) ? GetModelKind(info.pModel) : ModelKind::Other;

	DrawModelForVR(ecx, state, info, pCustomBoneToWorld);

	// What the player carries is drawn with the weapon, in the same pass
	if (kind == ModelKind::Viewmodel || kind == ModelKind::DualPistolsViewmodel)
		m_VR->DrawBodyModels(info.flags, info.pRenderable, info.entity_index);
}

namespace
{
	// What IVModelRender::DrawModelSetup fills (engine.dll of Left 4 Dead 2)
	struct DrawModelState
	{
		const void *studioHeader;
		void *hardwareData;
		void *renderable;
		const void *modelToWorld;
		void *decals;
		int drawFlags;
		int lod;
	};

	// The start of CModelRender::DrawModelSetup, to be sure it's the function at its place in the interface
	const unsigned char kDrawModelSetupStart[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x8B, 0x45, 0x10, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08 };
	const int kDrawModelSetupIndex = 18;
}

// Draws a model of the game that no entity draws, with the weapon's renderable for what the engine asks of one.
// IVModelRender::DrawModel can't be used: it asks that renderable for bones, and the weapon has more bones than the
// model, so it refuses. DrawModelSetup without STUDIO_RENDER prepares the model (header, mesh, detail level) without
// asking for them, and then the model is drawn with its own bones.
void Hooks::DrawBodyModel(int flags, void *renderable, int entityIndex, const void *bodyDraw)
{
	const VR::BodyModelDraw &draw = *(const VR::BodyModelDraw *)bodyDraw;
	const void *model = draw.model;
	const HandMath::Mat34 &frame = draw.frame;
	if (!model || !renderable || !m_Game->m_ModelRender || m_BodyModelsBroken)
		return;

	typedef bool(__thiscall *tDrawModelSetup)(void *thisptr, ModelRenderInfo_t &info, DrawModelState *state, void **bonesOut);
	const tDrawModelSetup setup = (tDrawModelSetup)(*(void ***)m_Game->m_ModelRender)[kDrawModelSetupIndex];
	if (!m_BodyModelChecked)
	{
		m_BodyModelChecked = true;
		if (memcmp((const void *)setup, kDrawModelSetupStart, sizeof(kDrawModelSetupStart)) != 0)
		{
			m_BodyModelsBroken = true;
			Diagnostics::Log("body items: DrawModelSetup isn't where it was expected in this version of the game, nothing is drawn on the body");
			return;
		}
	}

	ModelRenderInfo_t info;
	info.origin = Vector(frame.m[0][3], frame.m[1][3], frame.m[2][3]);
	info.angles = QAngle(0.0f, 0.0f, 0.0f);
	info.pRenderable = renderable;
	info.pModel = (model_t *)model;
	info.pModelToWorld = (const matrix3x4_t *)&frame;
	info.pLightingOffset = nullptr;
	info.pLightingOrigin = nullptr;
	info.flags = 0;
	info.entity_index = entityIndex;
	info.skin = 0;
	info.body = 0;
	info.hitboxset = 0;
	info.instance = 0xFFFF; // MODEL_INSTANCE_INVALID

	DrawModelState state = {};
	void *unusedBones = nullptr;
	if (!setup(m_Game->m_ModelRender, info, &state, &unusedBones) || !state.studioHeader || !state.hardwareData)
	{
		if (!m_LoggedBodyModelMiss)
			Diagnostics::Log("body items: the engine couldn't prepare %s yet", Studio::IsValid(state.studioHeader) ? Studio::ModelName(state.studioHeader) : "a model");
		m_LoggedBodyModelMiss = true;
		return;
	}

	const int boneCount = Studio::IsValid(state.studioHeader) ? Studio::BoneCount(state.studioHeader) : 0;
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return;

	alignas(16) unsigned char bones[sizeof(HandMath::Mat34) * Studio::kMaxBones];
	m_VR->BuildBodyModelBones(state.studioHeader, draw, bones);
	info.flags = flags;
	hkDrawModelExecute.fOriginal(m_Game->m_ModelRender, &state, info, bones);

	if (!m_LoggedBodyModel)
		Diagnostics::Log("body items: drawn with the weapon (%s)", Studio::ModelName(state.studioHeader));
	m_LoggedBodyModel = true;
}

void Hooks::DrawModelForVR(void *ecx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
	if (!m_VR->m_IsVREnabled || !info.pModel || !state)
		return hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);

	ModelKind kind = GetModelKind(info.pModel);

	if (kind == ModelKind::FirstPersonArms && !m_LoggedArmsDraw)
	{
		m_LoggedArmsDraw = true;
		Diagnostics::Log("hands: the arms are drawn with %s", pCustomBoneToWorld ? "their bones in the draw call" : "no bones in the draw call, they can't be changed");
	}

	if (kind == ModelKind::Viewmodel || kind == ModelKind::DualPistolsViewmodel)
		m_VR->m_ViewmodelHasTwoWeapons = kind == ModelKind::DualPistolsViewmodel;

	// The grenade has left the hand: the game is still playing the throw until it changes weapon
	if (kind == ModelKind::Viewmodel && m_VR->ThrownGrenadeHasLeftTheHand())
		return;

	// The left hand's item being used: the game holds it in its weapon hand, put at the left controller. The mod draws it
	// in the left hand instead (VR::GetLeftItemDraw), and the right hand empty (BuildFirstPersonHands). The weapon the
	// game puts away or takes out meanwhile isn't drawn either.
	// The right hand empty: its weapon is put away, the game's one is there only to draw the hands with (EmptyRightHand)
	if ((kind == ModelKind::Viewmodel || kind == ModelKind::DualPistolsViewmodel) && (m_VR->m_LeftUseActive || m_VR->m_RightHandEmpty))
		return;

	// l4d2vr_dual_test vmgrip: how the weapon's animation holds it, relative to the right hand (to hold it the same way in
	// the left one)
	if (kind == ModelKind::Viewmodel && pCustomBoneToWorld && m_VR->m_LogViewmodelGrip > 0)
	{
		--m_VR->m_LogViewmodelGrip;
		const void *studioHeader = *(const void **)state;
		const HandMath::Mat34 *bones = (const HandMath::Mat34 *)pCustomBoneToWorld;
		const int count = Studio::BoneCount(studioHeader);
		const int hand = Studio::FindBone(studioHeader, "ValveBiped.Bip01_R_Hand");
		if (hand >= 0 && hand < count)
		{
			const HandMath::Mat34 toHand = HandMath::InvertRigid(bones[hand]);
			for (int i = 0; i < count; ++i)
			{
				const char *name = Studio::BoneName(studioHeader, i);
				if (!strstr(name, "weapon") && !strstr(name, "Finger1") && !strstr(name, "Finger4") && !strstr(name, "Finger0") && i != hand)
					continue;
				const HandMath::Mat34 m = HandMath::Multiply(toHand, bones[i]);
				Diagnostics::Log("viewmodel grip: %s %s: origin %.2f %.2f %.2f, x %.2f %.2f %.2f, y %.2f %.2f %.2f, z %.2f %.2f %.2f", Studio::ModelName(studioHeader), name,
					m.m[0][3], m.m[1][3], m.m[2][3], m.m[0][0], m.m[1][0], m.m[2][0], m.m[0][1], m.m[1][1], m.m[2][1], m.m[0][2], m.m[1][2], m.m[2][2]);
			}
		}
	}

	// The weapon and the hand keep their pose while throwing with the arm, instead of the game's wind-up animation
	alignas(16) unsigned char throwPose[sizeof(HandMath::Mat34) * Studio::kMaxBones];
	if (pCustomBoneToWorld && (kind == ModelKind::Viewmodel || kind == ModelKind::FirstPersonArms))
		pCustomBoneToWorld = (void *)m_VR->ApplyThrowPose(*(const void **)state, pCustomBoneToWorld, throwPose);

	// Grenades, pills and adrenaline go with the hand to the controller
	alignas(16) unsigned char itemPose[sizeof(HandMath::Mat34) * Studio::kMaxBones];
	if (pCustomBoneToWorld && kind == ModelKind::Viewmodel)
		pCustomBoneToWorld = (void *)m_VR->ApplyItemInHand(*(const void **)state, pCustomBoneToWorld, itemPose);

	if (kind == ModelKind::Viewmodel && pCustomBoneToWorld)
	{
		Diagnostics::ScopedTimer timer(Diagnostics::Section_Models);

		alignas(16) unsigned char bones[sizeof(HandMath::Mat34) * Studio::kMaxBones];
		alignas(16) unsigned char detached[sizeof(HandMath::Mat34) * Studio::kMaxBones];
		bool drawDetached = false;

		if (m_VR->BuildReloadViewmodel(*(const void **)state, pCustomBoneToWorld, bones, detached, drawDetached))
		{
			hkDrawModelExecute.fOriginal(ecx, state, info, bones);
			if (drawDetached)
				hkDrawModelExecute.fOriginal(ecx, state, info, detached);
			return;
		}
	}

	if (pCustomBoneToWorld && kind != ModelKind::Other && kind != ModelKind::Viewmodel && kind != ModelKind::DualPistolsViewmodel)
	{
		Diagnostics::ScopedTimer timer(Diagnostics::Section_Models);

		alignas(16) unsigned char bones[sizeof(HandMath::Mat34) * Studio::kMaxBones];
		const void *studioHeader = *(const void **)state;

		bool changed = false;
		if (kind == ModelKind::FirstPersonArms)
			changed = m_VR->BuildFirstPersonHands(studioHeader, pCustomBoneToWorld, bones);
		else if (kind == ModelKind::Survivor)
			changed = m_VR->BuildRemotePlayerBones(info.entity_index, studioHeader, pCustomBoneToWorld, bones);
		else if (kind == ModelKind::WorldWeapon)
			changed = m_VR->BuildRemoteWeaponBones(info.entity_index, studioHeader, pCustomBoneToWorld, bones);

		if (changed)
			return hkDrawModelExecute.fOriginal(ecx, state, info, bones);
	}

	hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
}

void Hooks::dPushRenderTargetAndViewport(void *ecx, void *edx, ITexture *pTexture, ITexture *pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
	if (!m_VR->m_CreatedVRTextures || m_RenderingScope)
		return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

	if (m_PushHUDStep == 2)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	// RenderView calls PushRenderTargetAndViewport multiple times with different textures. 
	// When the call order goes PopRenderTargetAndViewport -> IsSplitScreen -> PrePushRenderTarget -> PushRenderTargetAndViewport,
	// then it pushed the HUD/GUI render target to the RT stack.
	if (m_PushHUDStep == 3)
	{
		pTexture = m_VR->m_HUDTexture;

		IMatRenderContext *renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		renderContext->ClearBuffers(false, true, true);

		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

		renderContext->OverrideAlphaWriteEnable(true, true);
		renderContext->ClearColor4ub(0, 0, 0, 0);
		renderContext->ClearBuffers(true, false);

		m_VR->m_RenderedHud = true;
		m_PushedHud = true;
	}
	else
	{
		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
	}
}

void Hooks::dPopRenderTargetAndViewport(void *ecx, void *edx)
{
	if (!m_VR->m_CreatedVRTextures || m_RenderingScope)
		return hkPopRenderTargetAndViewport.fOriginal(ecx);

	m_PushHUDStep = 0;

	if (m_PushedHud)
	{
		m_Game->m_MaterialSystem->GetRenderContext()->OverrideAlphaWriteEnable(false, true);
		m_Game->m_MaterialSystem->GetRenderContext()->ClearColor4ub(0, 0, 0, 255);
	}

	hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void Hooks::dVGui_Paint(void *ecx, void *edx, int mode)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkVgui_Paint.fOriginal(ecx, mode);

	// No HUD in the scope view
	if (m_RenderingScope)
		return;

	if (m_PushedHud)
		mode = PAINT_UIPANELS | PAINT_INGAMEPANELS;

	hkVgui_Paint.fOriginal(ecx, mode);
}

int Hooks::dIsSplitScreen()
{
	if (m_RenderingScope)
		return hkIsSplitScreen.fOriginal();

	if (m_PushHUDStep == 0)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkIsSplitScreen.fOriginal();
}

DWORD *Hooks::dPrePushRenderTarget(void *ecx, void *edx, int a2)
{
	if (m_RenderingScope)
		return hkPrePushRenderTarget.fOriginal(ecx, a2);

	if (m_PushHUDStep == 1)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkPrePushRenderTarget.fOriginal(ecx, a2);
}

// The regular game draws the viewmodel with its own, narrower field of view (fovViewmodel), so points attached to the
// viewmodel (laser sight, flashlight, muzzle flash, viewmodel particles) are converted between the viewmodel and the
// world projection by scaling their offset from the center of the screen.
// In VR both eyes draw the viewmodel with the world FOV (fovViewmodel == fov in dRenderView), so there is nothing to
// convert: the conversion only moved those points around depending on where the HMD was looking.
// (That made the laser trace start away from the weapon, so the dot wasn't on the beam.)
void Hooks::dFormatViewModelAttachment(void *player, Vector &origin, bool inverse)
{
	if (m_VR->m_IsVREnabled)
		return;

	hkFormatViewModelAttachment.fOriginal(player, origin, inverse);
}

namespace
{
	class ScopedUseAim
	{
	public:
		ScopedUseAim(UseAim &aim, void *player, const Vector &origin, const QAngle &angles) : m_Aim(aim), m_Previous(aim)
		{
			m_Aim.player = player;
			m_Aim.origin = origin;
			m_Aim.angles = angles;
		}

		~ScopedUseAim() { m_Aim = m_Previous; }

	private:
		UseAim &m_Aim;
		UseAim m_Previous;
	};
}

void Hooks::initRelayHook()
{
	HMODULE server = GetModuleHandleA("server.dll");
	typedef void *(__cdecl *tCreateInterface)(const char *, int *);
	tCreateInterface createInterface = server ? (tCreateInterface)GetProcAddress(server, "CreateInterface") : nullptr;
	if (!createInterface)
		return;

	int returnCode = 0;
	void *serverGameClients = createInterface("ServerGameClients003", &returnCode);
	if (!serverGameClients)
	{
		Diagnostics::Log("pose relay: no ServerGameClients003, this game won't forward poses when hosting");
		return;
	}

	// ClientCommand(edict_t *, const CCommand &)
	void **vtable = *(void ***)serverGameClients;
	hkServerClientCommand.createHook(vtable[5], &dServerClientCommand);
}

void __fastcall Hooks::dServerClientCommand(void *ecx, void *edx, edict_t *player, const void *command)
{
	if (m_Game->m_PoseRelay && m_Game->m_PoseRelay->HandleServerCommand(player, command))
		return;

	hkServerClientCommand.fOriginal(ecx, player, command);
}

// The game reloads an empty weapon by itself, and when the reload button is pressed. With manual reload only the button
// can start one. The client predicts weapons too, so both sides are stopped or the animation would start anyway.
void Hooks::initWeaponStateHooks()
{
	const Offsets *offsets = m_Game->m_Offsets;

	if (offsets->ServerDefaultReload.address)
		hkServerDefaultReload.createHook((LPVOID)offsets->ServerDefaultReload.address, &dServerDefaultReload);
	if (offsets->ClientDefaultReload.address)
		hkClientDefaultReload.createHook((LPVOID)offsets->ClientDefaultReload.address, &dClientDefaultReload);
	if (offsets->ServerShotgunReload.address)
		hkServerShotgunReload.createHook((LPVOID)offsets->ServerShotgunReload.address, &dServerShotgunReload);
	if (offsets->ClientShotgunReload.address)
		hkClientShotgunReload.createHook((LPVOID)offsets->ClientShotgunReload.address, &dClientShotgunReload);
	if (offsets->ServerSetHelpingHandState.address)
		hkServerSetHelpingHandState.createHook((LPVOID)offsets->ServerSetHelpingHandState.address, &dServerSetHelpingHandState);
	if (offsets->ClientSetHelpingHandState.address)
		hkClientSetHelpingHandState.createHook((LPVOID)offsets->ClientSetHelpingHandState.address, &dClientSetHelpingHandState);

	// Dual wield: a melee weapon in the left hand has a slot of its own
	DualWield::ServerInit(m_Game);
	if (offsets->ClientGetSlot.address && offsets->ServerGetSlot.address)
	{
		hkClientGetSlot.createHook((LPVOID)offsets->ClientGetSlot.address, &dClientGetSlot);
		hkServerGetSlot.createHook((LPVOID)offsets->ServerGetSlot.address, &dServerGetSlot);
	}
	else
	{
		Diagnostics::Log("dual wield: GetSlot not found in this game version, a weapon in the left hand replaces the right hand's of its slot");
	}
	if (offsets->ServerAddDualWeapon.address && offsets->ServerWeaponOfId.address)
	{
		hkServerAddDualWeapon.createHook((LPVOID)offsets->ServerAddDualWeapon.address, &dServerAddDualWeapon);
		hkServerWeaponOfId.createHook((LPVOID)offsets->ServerWeaponOfId.address, &dServerWeaponOfId);
		if (offsets->ServerWeaponById.address)
			hkServerWeaponById.createHook((LPVOID)offsets->ServerWeaponById.address, &dServerWeaponById);
		else
			Diagnostics::Log("dual wield: the weapon spawns' pistol lookup wasn't found in this game version, a pistol spawn doesn't give a second one");
	}
	else
	{
		Diagnostics::Log("dual wield: the pair of pistols wasn't found in this game version, a second pistol makes one");
	}
	if (!offsets->ServerSelectItem.address)
		Diagnostics::Log("dual wield: SelectItem not found in this game version, the hands can't change weapons");
	if (offsets->ClientParticleTracer.address)
		hkClientParticleTracer.createHook((LPVOID)offsets->ClientParticleTracer.address, &dClientParticleTracer);
	else
		Diagnostics::Log("left gun: the tracers weren't found in this game version, the left hand's shots show none");

	// The grenade launcher's grenade from the weapon in the hand: the target of the call found by its signature
	const uintptr_t createCall = offsets->GrenadeLauncherCreateCall.address;
	if (createCall && *(const uint8_t *)createCall == 0xE8)
		hkGrenadeLauncherCreate.createHook((LPVOID)(createCall + 5 + *(const int32_t *)(createCall + 1)), &dGrenadeLauncherCreate);
	else
		Diagnostics::Log("grenade launcher: its fire wasn't found in this game version, the grenade leaves from the eyes");
}

// The grenade launcher's grenade fired by a VR player leaves from the weapon in the hand, where it points. The game's
// velocity, relative to where the eyes look (forward with some lift), is turned the same way.
void *__cdecl Hooks::dGrenadeLauncherCreate(const Vector &origin, const QAngle &angles, const Vector &velocity, const Vector &angularVelocity, void *owner, int flags)
{
	bool isLocalPlayer;
	const int playerIndex = GetServerEntityIndex(owner);
	if (!IsVRThrower(playerIndex, isLocalPlayer))
		return hkGrenadeLauncherCreate.fOriginal(origin, angles, velocity, angularVelocity, owner, flags);

	Vector hand;
	QAngle aim;
	if (isLocalPlayer)
	{
		hand = m_VR->GetRightControllerAbsPos();
		aim = m_VR->GetRightControllerAbsAngle();
	}
	else
	{
		const Player &player = m_Game->m_PlayersVRInfo[playerIndex];
		hand = player.controllerPos;
		aim = player.controllerAngle;
	}

	Vector eyeForward, eyeRight, eyeUp, aimForward, aimRight, aimUp;
	QAngle::AngleVectors(angles, &eyeForward, &eyeRight, &eyeUp);
	QAngle::AngleVectors(aim, &aimForward, &aimRight, &aimUp);
	const Vector newVelocity = aimForward * DotProduct(velocity, eyeForward) + aimRight * DotProduct(velocity, eyeRight) + aimUp * DotProduct(velocity, eyeUp);

	// In front of the hand, unless a wall is between it and the eyes
	const float kMuzzleForward = 16.0f;
	Vector eye;
	hkEyePosition.fOriginal(owner, &eye);
	const Vector newOrigin = m_VR->GetGrenadeThrowOrigin(eye, hand + aimForward * kMuzzleForward, origin);

	static bool s_Logged = false;
	if (!s_Logged)
		Diagnostics::Log("grenade launcher: fired by player %d from the hand, %.0f units from the game's point", playerIndex, VectorLength(newOrigin - origin));
	s_Logged = true;

	return hkGrenadeLauncherCreate.fOriginal(newOrigin, aim, newVelocity, angularVelocity, owner, flags);
}

int __fastcall Hooks::dClientGetSlot(void *ecx, void *edx)
{
	int slot;
	if (DualWield::ClientGetSlot(ecx, slot))
		return slot;
	return hkClientGetSlot.fOriginal(ecx);
}

int __fastcall Hooks::dServerGetSlot(void *ecx, void *edx)
{
	int slot;
	if (DualWield::ServerGetSlot(ecx, slot))
		return slot;
	return hkServerGetSlot.fOriginal(ecx);
}

namespace
{
	// A VR player using something (dPlayerUse)
	void *g_VRUsePlayer = nullptr;
}

// A player who has a pistol and takes another one gets a pair of pistols. VR players hold the second one in the other
// hand, as a weapon of its own (dual_wield.cpp), and the left hand's slot can have any gun: theirs is never made a pair.
// Taking a pistol, the one they have isn't found (dServerWeaponOfId): it's taken like any other weapon and takes the place
// of the weapon of its slot, which goes to the ground. Other ways to a pair (the "give" command, a pair carried to the
// next map) do nothing.
bool __fastcall Hooks::dServerAddDualWeapon(void *ecx, void *edx)
{
	const int playerIndex = m_GetGrenadeOwner ? GetServerEntityIndex(m_GetGrenadeOwner(ecx)) : -1;
	bool isLocalPlayer;
	if (playerIndex < 1 || !IsVRThrower(playerIndex, isLocalPlayer))
		return hkServerAddDualWeapon.fOriginal(ecx);
	Diagnostics::Log("dual wield: player %d would have had a pair of pistols, not made", playerIndex);
	return false;
}

void *__fastcall Hooks::dServerWeaponOfId(void *ecx, void *edx, int weaponId)
{
	if (weaponId == C_WeaponCSBase::PISTOL && ecx == g_VRUsePlayer)
		return nullptr;
	return hkServerWeaponOfId.fOriginal(ecx, weaponId);
}

// The same for a weapon spawn (CWeaponSpawn::Use): it gives a new pistol instead
void *__fastcall Hooks::dServerWeaponById(void *ecx, void *edx, int weaponId)
{
	if (weaponId == C_WeaponCSBase::PISTOL && ecx == g_VRUsePlayer)
		return nullptr;
	return hkServerWeaponById.fOriginal(ecx, weaponId);
}

// The tracers of the left hand's gun's shot leave from its muzzle, not from the weapon in the right hand (FireClientBullets)
void __cdecl Hooks::dClientParticleTracer(const char *name, const Vector &start, const Vector &end, int entityIndex, int attachment, bool whiz)
{
	if (m_LeftShotStart)
	{
		static bool s_Logged = false;
		if (!s_Logged)
			Diagnostics::Log("left gun: its tracer (%s) leaves from its muzzle", name ? name : "?");
		s_Logged = true;
		hkClientParticleTracer.fOriginal(name, *m_LeftShotStart, end, 0, -1, whiz);
	}
	else
		hkClientParticleTracer.fOriginal(name, start, end, entityIndex, attachment, whiz);
}

// The tracer's effect makes the particle at its start, turned towards its end
void Hooks::ClientParticle(const char *name, const Vector &origin, const Vector &toward)
{
	if (!hkClientParticleTracer.pTarget || !name)
		return;
	__try
	{
		hkClientParticleTracer.fOriginal(name, origin, toward, 0, -1, false);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

bool Hooks::IsLocalPlayerWeapon(void *weapon)
{
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	return local && (void *)local->GetActiveWeapon() == weapon;
}

bool Hooks::ShouldBlockServerReload(void *weapon)
{
	if (!m_GetGrenadeOwner)
		return false;

	const int index = GetServerEntityIndex(m_GetGrenadeOwner(weapon));
	if (!m_Game->IsValidPlayerIndex(index))
		return false;

	const Player &player = m_Game->m_PlayersVRInfo[index];
	return player.isUsingVR && player.manualReload && !player.reloadButton;
}

bool Hooks::ShouldBlockClientReload(void *weapon)
{
	return m_VR->m_IsVREnabled && m_VR->m_ManualReloadEnabled && m_Game->m_PoseRelay->IsServerCapable()
		&& !m_VR->IsGameReloadButtonDown() && IsLocalPlayerWeapon(weapon);
}

bool __fastcall Hooks::dServerDefaultReload(void *ecx, void *edx, int clipSize1, int clipSize2, int activity)
{
	return !ShouldBlockServerReload(ecx) && hkServerDefaultReload.fOriginal(ecx, clipSize1, clipSize2, activity);
}

bool __fastcall Hooks::dClientDefaultReload(void *ecx, void *edx, int clipSize1, int clipSize2, int activity)
{
	return !ShouldBlockClientReload(ecx) && hkClientDefaultReload.fOriginal(ecx, clipSize1, clipSize2, activity);
}

bool __fastcall Hooks::dServerShotgunReload(void *ecx, void *edx)
{
	return !ShouldBlockServerReload(ecx) && hkServerShotgunReload.fOriginal(ecx);
}

bool __fastcall Hooks::dClientShotgunReload(void *ecx, void *edx)
{
	return !ShouldBlockClientReload(ecx) && hkClientShotgunReload.fOriginal(ecx);
}

// The "helping hand": near something to use, the game moves the weapon and reaches with the other hand. Anything other
// than state 0 (not reaching) is ignored for VR players, whose hands are their own.
void __fastcall Hooks::dServerSetHelpingHandState(void *ecx, void *edx, int state)
{
	if (state != 0 && m_GetGrenadeOwner)
	{
		bool isLocalPlayer;
		if (IsVRThrower(GetServerEntityIndex(m_GetGrenadeOwner(ecx)), isLocalPlayer))
			return;
	}

	hkServerSetHelpingHandState.fOriginal(ecx, state);
}

void __fastcall Hooks::dClientSetHelpingHandState(void *ecx, void *edx, int state)
{
	if (state != 0 && m_VR->m_IsVREnabled && IsLocalPlayerWeapon(ecx))
		return;

	hkClientSetHelpingHandState.fOriginal(ecx, state);
}

void Hooks::initUseHooks()
{
	const Offsets *offsets = m_Game->m_Offsets;

	if (offsets->FindUseEntity.address && offsets->ServerPlayerEyePosition.address && offsets->ServerPlayerEyeAngles.address)
	{
		hkFindUseEntity.createHook((LPVOID)offsets->FindUseEntity.address, &dFindUseEntity);
		hkServerPlayerEyePosition.createHook((LPVOID)offsets->ServerPlayerEyePosition.address, &dServerPlayerEyePosition);
		hkServerPlayerEyeAngles.createHook((LPVOID)offsets->ServerPlayerEyeAngles.address, &dServerPlayerEyeAngles);

		if (offsets->PlayerUse.address)
			hkPlayerUse.createHook((LPVOID)offsets->PlayerUse.address, &dPlayerUse);
	}
	else
	{
		Diagnostics::Log("interacting with the hand is off in this game version (server signatures)");
	}

	if (offsets->ClientFindUseEntity.address && offsets->ClientPlayerEyePosition.address && offsets->ClientPlayerEyeVectors.address)
	{
		hkClientFindUseEntity.createHook((LPVOID)offsets->ClientFindUseEntity.address, &dClientFindUseEntity);
		hkClientPlayerEyePosition.createHook((LPVOID)offsets->ClientPlayerEyePosition.address, &dClientPlayerEyePosition);
		hkClientPlayerEyeVectors.createHook((LPVOID)offsets->ClientPlayerEyeVectors.address, &dClientPlayerEyeVectors);
	}
}

bool Hooks::GetServerHandAim(void *player, Vector &origin, QAngle &angles)
{
	const int index = GetServerEntityIndex(player);
	if (!m_Game->IsValidPlayerIndex(index))
		return false;

	// The hand just grabbed something: this use is for that
	const Player &grabbing = m_Game->m_PlayersVRInfo[index];
	if (GetTickCount64() < grabbing.useAimUntil)
	{
		origin = grabbing.useAimOrigin;
		angles = grabbing.useAimAngles;
		return true;
	}

	if (!m_VR->m_InteractWithHand)
		return false;

	if (m_VR->m_IsVREnabled && index == m_Game->m_EngineClient->GetLocalPlayer())
	{
		origin = m_VR->GetRightControllerAbsPos();
		angles = m_VR->GetRightControllerAbsAngle();
		return true;
	}

	const Player &vrPlayer = m_Game->m_PlayersVRInfo[index];
	if (!vrPlayer.isUsingVR)
		return false;

	origin = vrPlayer.controllerPos;
	angles = vrPlayer.controllerAngle;
	return true;
}

void *__fastcall Hooks::dFindUseEntity(void *ecx, void *edx, float radius, float dotLimit, float defaultDotLimit, void *traceResult, void *extra)
{
	Vector origin;
	QAngle angles;
	if (GetServerHandAim(ecx, origin, angles))
	{
		ScopedUseAim aim(g_ServerUseAim, ecx, origin, angles);
		return hkFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
	}

	return hkFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
}

void __fastcall Hooks::dPlayerUse(void *ecx, void *edx, void *useEntity)
{
	// A melee weapon taken with the left hand doesn't replace the pistol (dual wield)
	const int playerIndex = GetServerEntityIndex(ecx);
	DualWield::ServerBeginUse(ecx, playerIndex);

	// A pistol taken by a VR player isn't made a pair with theirs (dServerWeaponOfId)
	bool isLocalPlayer;
	void *const usingBefore = g_VRUsePlayer;
	if (playerIndex > 0 && IsVRThrower(playerIndex, isLocalPlayer))
		g_VRUsePlayer = ecx;

	Vector origin;
	QAngle angles;
	if (GetServerHandAim(ecx, origin, angles))
	{
		ScopedUseAim aim(g_ServerUseAim, ecx, origin, angles);
		hkPlayerUse.fOriginal(ecx, useEntity);
	}
	else
	{
		hkPlayerUse.fOriginal(ecx, useEntity);
	}

	g_VRUsePlayer = usingBefore;
	DualWield::ServerEndUse(ecx, playerIndex);
}

Vector *__fastcall Hooks::dServerPlayerEyePosition(void *ecx, void *edx, Vector *eyePos)
{
	if (eyePos && g_ServerUseAim.player == ecx)
	{
		*eyePos = g_ServerUseAim.origin;
		return eyePos;
	}

	return hkServerPlayerEyePosition.fOriginal(ecx, eyePos);
}

const QAngle *__fastcall Hooks::dServerPlayerEyeAngles(void *ecx, void *edx)
{
	if (g_ServerUseAim.player == ecx)
		return &g_ServerUseAim.angles;

	return hkServerPlayerEyeAngles.fOriginal(ecx);
}

void *__fastcall Hooks::dClientFindUseEntity(void *ecx, void *edx, float radius, float dotLimit, float defaultDotLimit, void *traceResult, void *extra)
{
	Vector grabOrigin;
	QAngle grabAngles;
	if (m_VR->m_IsVREnabled && ecx == m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer()) && m_VR->GetGrabUseAim(grabOrigin, grabAngles))
	{
		ScopedUseAim aim(g_ClientUseAim, ecx, grabOrigin, grabAngles);
		return hkClientFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
	}

	if (m_VR->m_IsVREnabled && m_VR->m_InteractWithHand && ecx == m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer()))
	{
		ScopedUseAim aim(g_ClientUseAim, ecx, m_VR->GetRightControllerAbsPos(), m_VR->GetRightControllerAbsAngle());
		return hkClientFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
	}

	return hkClientFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
}

Vector *__fastcall Hooks::dClientPlayerEyePosition(void *ecx, void *edx, Vector *eyePos)
{
	if (eyePos && g_ClientUseAim.player == ecx)
	{
		*eyePos = g_ClientUseAim.origin;
		return eyePos;
	}

	return hkClientPlayerEyePosition.fOriginal(ecx, eyePos);
}

void __fastcall Hooks::dClientPlayerEyeVectors(void *ecx, void *edx, Vector *forward, Vector *right, Vector *up)
{
	if (g_ClientUseAim.player == ecx)
	{
		QAngle::AngleVectors(g_ClientUseAim.angles, forward, right, up);
		return;
	}

	hkClientPlayerEyeVectors.fOriginal(ecx, forward, right, up);
}

int Hooks::GetServerEntityIndex(void *entity)
{
	if (!entity)
		return -1;

	typedef int(__thiscall *tEntindex)(void *thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);
	return oEntindex(entity);
}

// The host's own player in VR, or a VR player who joined
bool Hooks::IsVRThrower(int playerIndex, bool &isLocalPlayer)
{
	isLocalPlayer = m_VR->m_IsVREnabled && playerIndex == m_Game->m_EngineClient->GetLocalPlayer();
	if (isLocalPlayer)
		return true;

	return playerIndex >= 0 && playerIndex < (int)m_Game->m_PlayersVRInfo.size() && m_Game->m_PlayersVRInfo[playerIndex].isUsingVR;
}

namespace
{
	// A throw received from a player who joined, that wasn't used yet
	bool HasJoinedPlayerThrow(const Player &player)
	{
		return player.throwPending && std::chrono::steady_clock::now() - player.throwTime < VR::kGrenadeThrowValidTime;
	}

	float PitchUp(const Vector &v)
	{
		return RAD2DEG(atan2f(v.z, sqrtf(v.x * v.x + v.y * v.y)));
	}
}

// CBaseCSGrenade::StartGrenadeThrow, on the server when the trigger is let go with a molotov, pipe bomb or bile jar
void __fastcall Hooks::dStartGrenadeThrow(void *ecx, void *edx)
{
	hkStartGrenadeThrow.fOriginal(ecx);

	bool isLocalPlayer;
	const int playerIndex = GetServerEntityIndex(m_GetGrenadeOwner(ecx));
	if (!IsVRThrower(playerIndex, isLocalPlayer))
		return;

	// EmitGrenade gets the origin and velocity of the throw
	HookEmitGrenade(ecx);

	// Thrown with the arm: throw now instead of 0.2 s later, when the hand would be somewhere else.
	// ThrowGrenade clears m_fThrowTime, so the game doesn't throw a second time.
	bool armThrow;
	if (isLocalPlayer)
	{
		armThrow = m_VR->HasPendingGestureThrow();
	}
	else
	{
		const Player &player = m_Game->m_PlayersVRInfo[playerIndex];
		armThrow = HasJoinedPlayerThrow(player) && player.throwGesture;
	}

	if (armThrow)
		m_ThrowGrenade(ecx);
}

namespace
{
	// Grenade classes whose EmitGrenade was replaced, with the original function
	struct EmitGrenadeHook
	{
		void **vtable;
		tEmitGrenade original;
	};
	EmitGrenadeHook g_EmitGrenadeHooks[8];
	int g_EmitGrenadeHookCount = 0;
}

// Only called from the server (game thread)
void Hooks::HookEmitGrenade(void *grenade)
{
	void **vtable = *(void ***)grenade;
	for (int i = 0; i < g_EmitGrenadeHookCount; ++i)
	{
		if (g_EmitGrenadeHooks[i].vtable == vtable)
			return;
	}

	if (g_EmitGrenadeHookCount == ARRAYSIZE(g_EmitGrenadeHooks))
		return;

	void **slot = (void **)((uintptr_t)vtable + m_EmitGrenadeVtableOffset);
	DWORD oldProtect;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect))
		return;

	g_EmitGrenadeHooks[g_EmitGrenadeHookCount++] = { vtable, (tEmitGrenade)*slot };
	*slot = (void *)&dEmitGrenade;
	VirtualProtect(slot, sizeof(void *), oldProtect, &oldProtect);
}

// CMolotov / CPipeBomb / CItem_VomitJar::EmitGrenade, called by CBaseCSGrenade::ThrowGrenade to create the projectile
void *__fastcall Hooks::dEmitGrenade(void *ecx, void *edx, VectorValue origin, VectorValue angles, VectorValue velocity, VectorValue angularImpulse, void *owner)
{
	tEmitGrenade original = nullptr;
	void **vtable = *(void ***)ecx;
	for (int i = 0; i < g_EmitGrenadeHookCount; ++i)
	{
		if (g_EmitGrenadeHooks[i].vtable == vtable)
			original = g_EmitGrenadeHooks[i].original;
	}
	if (!original)
		return nullptr;

	// The angular impulse is (600, random, 0) for a throw and zero when the grenade is dropped
	bool isLocalPlayer;
	const int playerIndex = GetServerEntityIndex(owner);
	if (angularImpulse.x != 0.0f && IsVRThrower(playerIndex, isLocalPlayer))
	{
		const Vector gameOrigin(origin.x, origin.y, origin.z);
		const Vector gameVelocity(velocity.x, velocity.y, velocity.z);
		Vector newOrigin, armVelocity;
		int ageMs = 0;
		bool replace = false;

		if (isLocalPlayer)
		{
			replace = m_VR->TakeGrenadeThrow(gameOrigin, newOrigin, armVelocity, ageMs);
		}
		else
		{
			// From the hand position the player sends with every usercmd, checked against walls from the eyes
			Player &player = m_Game->m_PlayersVRInfo[playerIndex];
			if (HasJoinedPlayerThrow(player))
			{
				player.throwPending = false;
				Vector eyePosition;
				hkEyePosition.fOriginal(owner, &eyePosition);
				newOrigin = m_VR->GetGrenadeThrowOrigin(eyePosition, player.controllerPos, gameOrigin);
				armVelocity = player.throwArmVelocity;
				ageMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - player.throwTime).count();
				replace = true;
			}
		}

		if (replace)
		{
			// Like the game: the throw plus the player's velocity (ThrowGrenade just updated it)
			const Vector playerVelocity = *(const Vector *)((uintptr_t)owner + m_AbsVelocityOffset);
			const Vector newVelocity = armVelocity + playerVelocity;

			VR::WriteGrenadeThrowLog("thrown by player %d (%s) %d ms after %s, %.0f units from the game's spawn point: %.0f units/s at %.0f deg up, player velocity %.0f. Game's throw: %.0f units/s at %.0f deg up",
				playerIndex, isLocalPlayer ? "this game" : "joined", ageMs, isLocalPlayer ? "the release" : "the throw arrived",
				VectorLength(newOrigin - gameOrigin), VectorLength(newVelocity), PitchUp(newVelocity), VectorLength(playerVelocity),
				VectorLength(gameVelocity), PitchUp(gameVelocity));

			origin = { newOrigin.x, newOrigin.y, newOrigin.z };
			velocity = { newVelocity.x, newVelocity.y, newVelocity.z };

			// The game's spin is for a full throw: a gentle toss or a drop spins less
			const float spin = std::clamp(VectorLength(armVelocity) / VR::kGameThrowSpeed, 0.0f, 1.0f);
			angularImpulse = { angularImpulse.x * spin, angularImpulse.y * spin, angularImpulse.z * spin };
		}
	}

	return original(ecx, origin, angles, velocity, angularImpulse, owner);
}