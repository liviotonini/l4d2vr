#pragma once
#include "openvr.h"
#include <initializer_list>
#include <vector>

// One vibration of a haptic effect
struct HapticPulse
{
	float delay;      // seconds after the start of the effect
	float duration;   // seconds
	float frequency;  // Hz
	float amplitude;  // 0 to 1
};

// A vibration made of one or more pulses, e.g. { { 0.0f, 0.04f, 160.0f, 0.3f } }
struct HapticEffect
{
	std::vector<HapticPulse> pulses;

	HapticEffect(std::initializer_list<HapticPulse> pulseList) : pulses(pulseList) {}
};

// Effects shared by the features that give feedback through the controllers
namespace HapticEffects
{
	// One short, gentle tap: something was done
	inline const HapticEffect Confirm = { { 0.0f, 0.04f, 160.0f, 0.3f } };
	// Two short, gentle taps: something was cancelled
	inline const HapticEffect Cancel = { { 0.0f, 0.035f, 120.0f, 0.3f }, { 0.09f, 0.035f, 120.0f, 0.3f } };
}

// Controller vibrations through SteamVR Input: the /actions/base/out/vibration_left and vibration_right actions, which
// the default bindings (VR\SteamVRActionManifest) send to the haptics of each hand.
// Use it from VR::Update, the thread of the other SteamVR Input calls.
//
//   m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Cancel);
//   m_Haptics.Play(Haptics::Hand::WeaponHand, { { 0.0f, 0.1f, 200.0f, 0.5f } });
class Haptics
{
public:
	// The weapon hand is the right hand, or the left one with LeftHanded
	enum class Hand { WeaponHand, OtherHand, BothHands };

	void Init(vr::IVRInput *input, const bool *leftHanded);
	void Play(Hand hand, const HapticEffect &effect);

private:
	void PlayOnAction(vr::VRActionHandle_t action, const HapticEffect &effect);

	vr::IVRInput *m_Input = nullptr;
	vr::VRActionHandle_t m_LeftHandAction = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_RightHandAction = vr::k_ulInvalidActionHandle;
	const bool *m_LeftHanded = nullptr;
};
