#pragma once
#include <string>
#include <vector>
#include "hand_math.h"

// What the mod needs to know about the skeleton of a model that has hands (first person arms, survivors).
// Everything comes from the model itself, so custom survivors keep working.
class HandRig
{
public:
	enum Side { Left, Right, SideCount };
	enum Finger { Thumb, Index, Middle, Ring, Pinky, FingerCount };

	struct BoneData
	{
		std::string name;
		int parent;
		HandMath::Mat34 poseToBone;
	};

	void Build(const std::vector<BoneData> &bones);

	bool HasHand(Side side) const { return m_Sides[side].hand >= 0; }
	int HandBone(Side side) const { return m_Sides[side].hand; }
	int UpperArmBone(Side side) const { return m_Sides[side].upperArm; }
	int ForearmBone(Side side) const { return m_Sides[side].forearm; }
	int HeadBone() const { return m_Head; }

	// Moves the arm so the hand ends at `hand`, bending the elbow. A target out of reach leaves the arm stretched towards it.
	void SolveArm(Side side, const HandMath::Mat34 &hand, HandMath::Mat34 *bones) const;

	// The arm is folded into the wrist: vertices skinned to it collapse to one point, so only the hand is visible.
	// Costs nothing compared to drawing the model again with clip planes, and works from any angle.
	void CollapseArm(Side side, HandMath::Mat34 *bones) const;

	// The matrix of the hand bone with the fingers pointing along `forward`, the thumb side towards `up` and the
	// wrist at `wrist`
	HandMath::Mat34 HandFromFrame(Side side, const HandMath::Vec3 &forward, const HandMath::Vec3 &up, const HandMath::Vec3 &wrist) const;

	// Places the hand and its fingers. curls: 0 = open, 1 = closed. The rest pose of the model is curl kRelaxedCurl.
	void PoseHand(Side side, const HandMath::Mat34 &hand, const float curls[FingerCount], HandMath::Mat34 *bones) const;

	// Copies the transforms of a hand from `from` to `to`, gradually
	void BlendHand(Side side, const HandMath::Mat34 *from, const HandMath::Mat34 *to, float amount, HandMath::Mat34 *bones) const;

	static constexpr float kRelaxedCurl = 0.35f;

private:
	struct Joint
	{
		int bone;
		int parent;
		HandMath::Mat34 local;      // relative to the parent, in the rest pose
		int finger;                 // -1 for helper bones, they just follow their parent
		int segment;                // 0, 1, 2 from the palm
		HandMath::Vec3 curlAxis;    // in the coordinates of the joint
	};

	struct SideData
	{
		int hand = -1;
		int forearm = -1;
		int upperArm = -1;
		HandMath::Mat34 restHand;
		HandMath::Mat34 restFrame;  // fingers, thumb side, palm
		std::vector<Joint> joints;  // parents always before their children
		std::vector<int> arm;       // bones folded into the wrist
		std::vector<int> upperGroup;   // bones that move with the upper arm
		std::vector<int> forearmGroup; // and with the forearm
	};

	SideData m_Sides[SideCount];
	int m_Head = -1;
};
