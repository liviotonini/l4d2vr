#pragma once
#include <cstdint>
#include <vector>
#include "hand_rig.h"

// Read-only access to a studiohdr_t of the game (model versions 44 to 49, the ones Left 4 Dead 2 uses).
// The offsets were checked against the game's own files.
namespace Studio
{
	inline bool IsValid(const void *header)
	{
		if (!header)
			return false;
		const int32_t id = *(const int32_t *)header;
		const int32_t version = *((const int32_t *)header + 1);
		return id == 0x54534449 /* IDST */ && version >= 44 && version <= 49;
	}

	inline const char *ModelName(const void *header) { return (const char *)header + 12; }
	inline int BoneCount(const void *header) { return *(const int32_t *)((const uint8_t *)header + 156); }

	inline const uint8_t *BoneAt(const void *header, int index)
	{
		const int32_t boneIndex = *(const int32_t *)((const uint8_t *)header + 160);
		return (const uint8_t *)header + boneIndex + index * 216;
	}

	inline const char *BoneName(const void *header, int index)
	{
		const uint8_t *bone = BoneAt(header, index);
		return (const char *)bone + *(const int32_t *)bone;
	}

	inline int BoneParent(const void *header, int index) { return *(const int32_t *)(BoneAt(header, index) + 4); }

	inline const HandMath::Mat34 &PoseToBone(const void *header, int index) { return *(const HandMath::Mat34 *)(BoneAt(header, index) + 96); }

	inline std::vector<HandRig::BoneData> ReadBones(const void *header)
	{
		std::vector<HandRig::BoneData> bones;
		const int count = BoneCount(header);
		bones.reserve(count);
		for (int i = 0; i < count; ++i)
			bones.push_back({ BoneName(header, i), BoneParent(header, i), PoseToBone(header, i) });
		return bones;
	}

	// Bone index by name, -1 if the model doesn't have it. Slow, call it once per model.
	inline int FindBone(const void *header, const char *name)
	{
		const int count = BoneCount(header);
		for (int i = 0; i < count; ++i)
		{
			if (_stricmp(BoneName(header, i), name) == 0)
				return i;
		}
		return -1;
	}

	const int kMaxBones = 128;
}
