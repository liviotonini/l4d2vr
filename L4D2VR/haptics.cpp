#include "haptics.h"

void Haptics::Init(vr::IVRInput *input, const bool *leftHanded)
{
	m_Input = input;
	m_LeftHanded = leftHanded;

	if (!input || input->GetActionHandle("/actions/base/out/vibration_left", &m_LeftHandAction) != vr::VRInputError_None)
		m_LeftHandAction = vr::k_ulInvalidActionHandle;
	if (!input || input->GetActionHandle("/actions/base/out/vibration_right", &m_RightHandAction) != vr::VRInputError_None)
		m_RightHandAction = vr::k_ulInvalidActionHandle;
}

void Haptics::Play(Hand hand, const HapticEffect &effect)
{
	if (!m_Input)
		return;

	const bool leftHanded = m_LeftHanded && *m_LeftHanded;
	const bool leftHand = hand == Hand::BothHands || (hand == Hand::WeaponHand) == leftHanded;
	const bool rightHand = hand == Hand::BothHands || (hand == Hand::WeaponHand) != leftHanded;

	if (leftHand)
		PlayOnAction(m_LeftHandAction, effect);
	if (rightHand)
		PlayOnAction(m_RightHandAction, effect);
}

void Haptics::PlayOnAction(vr::VRActionHandle_t action, const HapticEffect &effect)
{
	if (action == vr::k_ulInvalidActionHandle)
		return;

	// SteamVR schedules the later pulses itself
	for (const HapticPulse &pulse : effect.pulses)
		m_Input->TriggerHapticVibrationAction(action, pulse.delay, pulse.duration, pulse.frequency, pulse.amplitude, vr::k_ulInvalidInputValueHandle);
}
