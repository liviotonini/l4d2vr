#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include "sdk.h"
#include "game.h"
#include "wrist_hud_draw.h"

namespace
{
	// Client netvars
	const int kTeamOffset = 0xE4;
	const int kHealthOffset = 0xEC;
	const int kLifeStateOffset = 0x147;
	const int kClipOffset = 0x984;
	const int kAmmoTypeOffset = 0x97C;
	const int kAmmoArrayOffset = 0xF24;
	const int kSurvivorCharacterOffset = 0x1C8C;
	const int kHealthBufferOffset = 0x1FAC;
	const int kHealthBufferTimeOffset = 0x1FB0;
	const int kIsOnThirdStrikeOffset = 0x1EC0;
	const int kIsIncapacitatedOffset = 0x1EA9;

	const int kTeamSurvivors = 2;
	const float kTemporaryHealthDecay = 0.27f; // pain_pills_decay_rate, health per second
	const ULONGLONG kRefreshIntervalMs = 250;
	const int kMaxUploadFailures = 5;

	// Width on the wrist, and where it is from the wrist like a watch, meters: towards the elbow and out of the back of the hand
	const float kWidthMeters = 0.1f;
	const float kTowardsElbow = 0.04f;
	const float kAboveTheWrist = 0.04f;
	const float kAlpha = 1.0f; // only the panel is see-through, that's in the image

	template <typename T>
	T Read(const void *entity, int offset)
	{
		return *(const T *)((const uint8_t *)entity + offset);
	}

	const int kIsHangingFromLedgeOffset = 0x25EC;
	const int kTongueOwnerOffset = 0x1F6C;
	const int kPummelAttackerOffset = 0x2720;
	const int kCarryAttackerOffset = 0x2714;
	const int kPounceAttackerOffset = 0x272C;
	const int kJockeyAttackerOffset = 0x274C;
	const int kMissionZombieKillsOffset = 0x24AC;
	const int kCheckpointZombieKillsOffset = 0x2488;
	const int kZombieKillsMaxIndex = 8; // 0 = common infected, 1 to 8 = special infected

	const char *SurvivorName(int character)
	{
		// In the order of m_survivorCharacter
		static const char *const kNames[] = { "NICK", "ROCHELLE", "COACH", "ELLIS", "BILL", "ZOEY", "FRANCIS", "LOUIS" };
		return character >= 0 && character < 8 ? kNames[character] : "?";
	}

	bool IsValidHandle(int handle)
	{
		return handle != 0 && handle != -1;
	}

	bool IsHeldBySpecialInfected(const void *player)
	{
		for (int offset : { kTongueOwnerOffset, kPummelAttackerOffset, kCarryAttackerOffset, kPounceAttackerOffset, kJockeyAttackerOffset })
		{
			if (IsValidHandle(Read<int>(player, offset)))
				return true;
		}
		return false;
	}

	const char *ItemAbbreviation(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::MOLOTOV: return "MOL";
		case C_WeaponCSBase::PIPE_BOMB: return "PIP";
		case C_WeaponCSBase::VOMITJAR: return "BIL";
		case C_WeaponCSBase::FIRST_AID_KIT: return "FAK";
		case C_WeaponCSBase::DEFIBRILLATOR: return "DEF";
		case C_WeaponCSBase::AMMO_PACK: return "AMP";
		case C_WeaponCSBase::INCENDIARY_AMMO: return "INC";
		case C_WeaponCSBase::FRAG_AMMO: return "EXP";
		case C_WeaponCSBase::PAIN_PILLS: return "PIL";
		case C_WeaponCSBase::ADRENALINE: return "ADR";
		default: return "";
		}
	}

	void ReadKills(const void *player, int baseOffset, int &common, int &special)
	{
		common = std::max(Read<int>(player, baseOffset), 0);
		special = 0;
		for (int i = 1; i <= kZombieKillsMaxIndex; ++i)
			special += std::max(Read<int>(player, baseOffset + i * 4), 0);
	}

	// The game gives the temporary health and when it was received, and subtracts what decayed since then
	struct TemporaryHealthClock
	{
		float buffer = 0.0f;
		float bufferTime = 0.0f;
		ULONGLONG changedAt = 0;
	};

	TemporaryHealthClock g_TemporaryHealth[65];

	int TemporaryHealth(int index, const void *player, ULONGLONG now)
	{
		const float buffer = Read<float>(player, kHealthBufferOffset);
		const float bufferTime = Read<float>(player, kHealthBufferTimeOffset);
		if (!std::isfinite(buffer) || buffer <= 0.0f || index < 0 || index >= 65)
			return 0;

		TemporaryHealthClock &clock = g_TemporaryHealth[index];
		if (buffer != clock.buffer || bufferTime != clock.bufferTime)
		{
			clock.buffer = buffer;
			clock.bufferTime = bufferTime;
			clock.changedAt = now;
		}

		const float seconds = (now - clock.changedAt) * 0.001f;
		return (int)std::max(std::ceil(buffer - seconds * kTemporaryHealthDecay), 0.0f);
	}
}

void VR::HideWristHud()
{
	if (m_WristHudVisible && m_WristHudHandle != vr::k_ulOverlayHandleInvalid)
		m_Overlay->HideOverlay(m_WristHudHandle);
	m_WristHudVisible = false;
}

// Called every frame. SteamVR is only told about what changed: the image a few times per second at most, the
// transform when the controller changes.
void VR::UpdateWristHud()
{
	if (!m_WristHudEnabled || !m_Game->m_EngineClient->IsInGame() || m_LeftControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid)
	{
		HideWristHud();
		return;
	}

	const int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(localIndex);
	if (!local || Read<uint8_t>(local, kLifeStateOffset) != 0 || Read<int>(local, kTeamOffset) != kTeamSurvivors)
	{
		HideWristHud();
		return;
	}

	if (m_WristHudHandle == vr::k_ulOverlayHandleInvalid)
	{
		if (m_Overlay->CreateOverlay("WristHudOverlayKey", "WristHudOverlay", &m_WristHudHandle) != vr::VROverlayError_None)
		{
			m_WristHudHandle = vr::k_ulOverlayHandleInvalid;
			return;
		}

		m_Overlay->SetOverlayWidthInMeters(m_WristHudHandle, kWidthMeters);
		m_Overlay->SetOverlayAlpha(m_WristHudHandle, kAlpha);
		m_WristHudDevice = vr::k_unTrackedDeviceIndexInvalid;
		m_WristHudDrawn = false;
		m_WristHudVisible = false;
	}

	if (m_WristHudDevice != m_LeftControllerDeviceIndex)
	{
		// Worn like a watch, in the frame of the controller (x right, y up, z back). The hand holds it tilted: the fingers
		// go along `f`, the thumb's side along `u`. The face looks out of the back of the hand and reads across the arm.
		const float radians = kHandGripTilt * 3.14159265f / 180.0f;
		const float s = sinf(radians), c = cosf(radians);
		const HandMath::Vec3 f(0.0f, s, -c);
		const HandMath::Vec3 u(0.0f, c, s);
		const HandMath::Vec3 wrist = f * m_HandOffsetMeters.x + u * m_HandOffsetMeters.z + HandMath::Vec3(m_HandOffsetMeters.y, 0.0f, 0.0f);

		// The back of the left hand faces -x, of the right one +x
		const HandMath::Vec3 back(m_LeftHanded ? 1.0f : -1.0f, 0.0f, 0.0f);
		// To read it, the fingers are to the right on the left arm, and to the left on the right one
		const HandMath::Vec3 across = m_LeftHanded ? f * -1.0f : f;
		const HandMath::Vec3 top = HandMath::Cross(back, across);
		const HandMath::Vec3 position = wrist - f * kTowardsElbow + back * kAboveTheWrist;

		vr::HmdMatrix34_t transform =
		{
			across.x, top.x, back.x, position.x,
			across.y, top.y, back.y, position.y,
			across.z, top.z, back.z, position.z
		};
		m_Overlay->SetOverlayTransformTrackedDeviceRelative(m_WristHudHandle, m_LeftControllerDeviceIndex, &transform);
		m_WristHudDevice = m_LeftControllerDeviceIndex;
	}

	const ULONGLONG now = GetTickCount64();
	if (!m_WristHudDrawn || now - m_WristHudLastRefresh >= kRefreshIntervalMs)
	{
		m_WristHudLastRefresh = now;

		WristHud::State state;
		state.health = Read<int>(local, kHealthOffset);
		state.temporary = TemporaryHealth(localIndex, local, now);
		state.incapacitated = Read<uint8_t>(local, kIsIncapacitatedOffset) != 0;
		state.ledge = Read<uint8_t>(local, kIsHangingFromLedgeOffset) != 0;
		state.lastLife = Read<uint8_t>(local, kIsOnThirdStrikeOffset) != 0;

		if (const C_BaseCombatWeapon *weapon = local->GetActiveWeapon())
		{
			const int slot = GetWeaponSlot(m_ActiveWeaponId);
			const bool isGun = slot == 1 || m_ActiveWeaponId == C_WeaponCSBase::PISTOL || m_ActiveWeaponId == C_WeaponCSBase::MAGNUM;
			const int clip = Read<int>(weapon, kClipOffset);
			if (isGun && clip >= 0)
			{
				state.hasAmmo = true;
				state.clip = clip;

				int reserve = 0;
				const int ammoType = Read<int>(weapon, kAmmoTypeOffset);
				if (slot == 1 && ammoType >= 0 && ammoType < 32)
				{
					reserve = Read<int>(local, kAmmoArrayOffset + ammoType * 4);
					state.reserve = reserve;
				}

				// The numbers turn red relative to the most this weapon has had
				if (m_ActiveWeaponId != m_WristHudWeaponId)
				{
					m_WristHudWeaponId = m_ActiveWeaponId;
					m_WristHudMaxClip = 0;
					m_WristHudMaxReserve = 0;
				}
				m_WristHudMaxClip = std::max(m_WristHudMaxClip, clip);
				m_WristHudMaxReserve = std::max(m_WristHudMaxReserve, reserve);
				state.clipLowAt = std::max(1, (m_WristHudMaxClip + 2) / 3);
				state.reserveLowAt = std::max(1, (m_WristHudMaxReserve + 4) / 5);
			}
		}

		// Grenade, medkit and pills
		for (int slot = 2; slot <= 4 && state.itemCount < WristHud::kMaxItems; ++slot)
		{
			C_WeaponCSBase *item = (C_WeaponCSBase *)local->Weapon_GetSlot(slot);
			if (!item || Read<int>(item, kClipOffset) == 0)
				continue;

			const char *abbreviation = ItemAbbreviation(item->GetWeaponID());
			if (abbreviation[0])
				snprintf(state.items[state.itemCount++], sizeof(state.items[0]), "%s", abbreviation);
		}

		int missionCommon, missionSpecial, checkpointCommon, checkpointSpecial;
		ReadKills(local, kMissionZombieKillsOffset, missionCommon, missionSpecial);
		ReadKills(local, kCheckpointZombieKillsOffset, checkpointCommon, checkpointSpecial);
		const bool useCheckpoint = missionCommon + missionSpecial == 0 && checkpointCommon + checkpointSpecial > 0;
		state.commonKills = useCheckpoint ? checkpointCommon : missionCommon;
		state.specialKills = useCheckpoint ? checkpointSpecial : missionSpecial;

		for (int i = 1; i <= 32 && state.teammateCount < WristHud::kMaxTeammates; ++i)
		{
			if (i == localIndex)
				continue;

			const void *player = m_Game->GetClientEntity(i);
			if (!player || Read<int>(player, kTeamOffset) != kTeamSurvivors || Read<uint8_t>(player, kLifeStateOffset) != 0)
				continue;

			WristHud::Teammate &mate = state.teammates[state.teammateCount++];
			mate.health = Read<int>(player, kHealthOffset);
			mate.temporary = TemporaryHealth(i, player, now);
			mate.incapacitated = Read<uint8_t>(player, kIsIncapacitatedOffset) != 0;
			mate.ledge = Read<uint8_t>(player, kIsHangingFromLedgeOffset) != 0;
			mate.lastLife = Read<uint8_t>(player, kIsOnThirdStrikeOffset) != 0;
			mate.controlled = IsHeldBySpecialInfected(player);

			player_info_t info;
			if (m_Game->m_EngineClient->GetPlayerInfo(i, &info) && info.name[0])
				snprintf(mate.name, sizeof(mate.name), "%s", info.name);
			else
				snprintf(mate.name, sizeof(mate.name), "%s", SurvivorName(Read<int>(player, kSurvivorCharacterOffset)));
		}

		if (!m_WristHudDrawn || state != *(const WristHud::State *)m_WristHudLastState)
		{
			m_WristHudPixels.resize(WristHud::kWidth * WristHud::kHeight * 4);
			WristHud::Draw(state, m_WristHudPixels.data());

			const vr::EVROverlayError error = m_Overlay->SetOverlayRaw(m_WristHudHandle, m_WristHudPixels.data(), WristHud::kWidth, WristHud::kHeight, 4);
			if (error != vr::VROverlayError_None)
			{
				// If the upload keeps failing the overlay is recreated
				if (++m_WristHudUploadFailures >= kMaxUploadFailures)
				{
					m_Overlay->DestroyOverlay(m_WristHudHandle);
					m_WristHudHandle = vr::k_ulOverlayHandleInvalid;
					m_WristHudUploadFailures = 0;
				}
				return;
			}

			m_WristHudUploadFailures = 0;
			m_WristHudDrawn = true;
			static_assert(sizeof(state) <= sizeof(m_WristHudLastState), "m_WristHudLastState is too small");
			memcpy(m_WristHudLastState, &state, sizeof(state));
		}
	}

	if (!m_WristHudVisible && m_WristHudDrawn)
	{
		m_Overlay->ShowOverlay(m_WristHudHandle);
		m_WristHudVisible = true;
	}
}
