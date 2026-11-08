#include "hooks.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "vr.h"
#include "offsets.h"
#include <algorithm>
#include <iostream>

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
	else if (m_Game->m_PlayersVRInfo[playerId].isUsingVR)
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


float __fastcall Hooks::dProcessUsercmds(void *ecx, void *edx, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused)
{
	// Function pointer for CBaseEntity::entindex
	typedef int(__thiscall *tEntindex)(void *thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

	IServerUnknown * pUnknown = player->m_pUnk;
	Server_BaseEntity *pPlayer = (Server_BaseEntity*)pUnknown->GetBaseEntity();

	int index = oEntindex(pPlayer);
	m_Game->m_CurrentUsercmdID = index;

	float result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);

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
				if (m_Game->m_PlayersVRInfo[index].isNewSwing)
				{
					m_Game->m_PlayersVRInfo[index].isNewSwing = false;
					curWep->entitiesHitThisSwing = 0;
				}

				typedef void *(__thiscall *tGetMeleeWepInfo)(void *thisptr);
				static tGetMeleeWepInfo oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfo.address);
				void *meleeWepInfo = oGetMeleeWepInfo(curWep);

				Vector initialForward, initialRight, initialUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].prevControllerAngle, &initialForward, &initialRight, &initialUp);
				Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0);
				VectorNormalize(initialMeleeDirection);

				Vector finalForward, finalRight, finalUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].controllerAngle, &finalForward, &finalRight, &finalUp);
				Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0);
				VectorNormalize(finalMeleeDirection);

				Vector pivot;
				CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
				VectorNormalize(pivot);

				float swingAngle = acos(DotProduct(initialMeleeDirection, finalMeleeDirection)) * 180 / 3.14159265;

				m_Game->m_Hooks->hkGetPrimaryAttackActivity.fOriginal(curWep, meleeWepInfo); // Needed to call TestMeleeSwingCollision

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
			}
		}
	}
	else
	{
		m_Game->m_PlayersVRInfo[index].isNewSwing = true;
	}

	m_Game->m_PlayersVRInfo[index].prevControllerAngle = m_Game->m_PlayersVRInfo[index].controllerAngle;

	return result;
}

int Hooks::dReadUsercmd(void *buf, CUserCmd *move, CUserCmd *from)
{
	hkReadUsercmd.fOriginal(buf, move, from);

	int i = m_Game->m_CurrentUsercmdID;
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

		if (VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > m_VR->m_MeleeSwingVelocityRequired)
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
	Vector *result = hkEyePosition.fOriginal(ecx, eyePos);

	if (m_Game->m_PerformingMelee)
	{
		int i = m_Game->m_CurrentUsercmdID;
		*result = m_Game->m_PlayersVRInfo[i].controllerPos;
	}

	return result;
}

void Hooks::dDrawModelExecute(void *ecx, void *edx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
	if (m_Game->m_SwitchedWeapons)
		m_Game->m_CachedArmsModel = false;

	bool hideArms = m_Game->m_IsMeleeWeaponActive || m_VR->m_HideArms;
	
	if (info.pModel && hideArms && !m_Game->m_CachedArmsModel)
	{
		std::string modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);
		if (modelName.find("/arms/") != std::string::npos)
		{
			m_Game->m_ArmsMaterial = m_Game->m_MaterialSystem->FindMaterial(modelName.c_str(), "Model textures");
			m_Game->m_ArmsModel = info.pModel;
			m_Game->m_CachedArmsModel = true;
		}
	}

	if (info.pModel && info.pModel == m_Game->m_ArmsModel && hideArms)
	{
		m_Game->m_ArmsMaterial->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		m_Game->m_ModelRender->ForcedMaterialOverride(m_Game->m_ArmsMaterial);
		hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
		m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
		return;
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