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

	// Width on the wrist and where it is on the controller, meters and degrees
	const float kWidthMeters = 0.1f;
	const float kOffsetX = 0.01f;
	const float kOffsetY = 0.01f;
	const float kOffsetZ = 0.0f;
	const float kTiltDegrees = -75.0f;
	const float kAlpha = 0.6f;

	template <typename T>
	T Read(const void *entity, int offset)
	{
		return *(const T *)((const uint8_t *)entity + offset);
	}

	char SurvivorLetter(int character)
	{
		// In the order of m_survivorCharacter: Nick, Rochelle, Coach, Ellis, Bill, Zoey, Francis, Louis
		static const char kLetters[] = { 'N', 'R', 'C', 'E', 'B', 'Z', 'F', 'L' };
		return character >= 0 && character < 8 ? kLetters[character] : '?';
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
		const float radians = kTiltDegrees * 3.14159265f / 180.0f;
		const float s = sinf(radians), c = cosf(radians);
		vr::HmdMatrix34_t transform =
		{
			1.0f, 0.0f, 0.0f, kOffsetX,
			0.0f, c,    -s,   kOffsetY,
			0.0f, s,    c,    kOffsetZ
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
				const int ammoType = Read<int>(weapon, kAmmoTypeOffset);
				if (slot == 1 && ammoType >= 0 && ammoType < 32)
					state.reserve = Read<int>(local, kAmmoArrayOffset + ammoType * 4);
			}
		}

		for (int i = 1; i <= 32 && state.teammateCount < WristHud::kMaxTeammates; ++i)
		{
			if (i == localIndex)
				continue;

			const void *player = m_Game->GetClientEntity(i);
			if (!player || Read<int>(player, kTeamOffset) != kTeamSurvivors || Read<uint8_t>(player, kLifeStateOffset) != 0)
				continue;

			WristHud::Teammate &mate = state.teammates[state.teammateCount++];
			mate.letter = SurvivorLetter(Read<int>(player, kSurvivorCharacterOffset));
			mate.health = Read<int>(player, kHealthOffset);
			mate.temporary = TemporaryHealth(i, player, now);
			mate.incapacitated = Read<uint8_t>(player, kIsIncapacitatedOffset) != 0;
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
