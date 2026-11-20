#pragma once
#include <cstdint>
#include <cstring>
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

	// The sounds of working the action (pump, bolt) in the fire animations of a model are switched on or off, in the copy
	// of the model the game has in memory. The events are the old style ones, which the client only plays from 5000 up.
	// Returns how many were found.
	inline int SetFireActionSounds(const void *header, bool on)
	{
		const int kPlaySoundEvent = 5004;
		const int kNewStyleEvent = 1024;
		const int sequenceSize = 212, eventSize = 80;

		const uint8_t *base = (const uint8_t *)header;
		const int32_t sequences = *(const int32_t *)(base + 188);
		const int32_t sequenceIndex = *(const int32_t *)(base + 192);

		int found = 0;
		for (int s = 0; s < sequences; ++s)
		{
			uint8_t *sequence = (uint8_t *)base + sequenceIndex + s * sequenceSize;
			const char *activity = (const char *)sequence + *(const int32_t *)(sequence + 8);
			if (!strstr(activity, "PRIMARYATTACK"))
				continue;

			const int32_t events = *(const int32_t *)(sequence + 24);
			const int32_t eventIndex = *(const int32_t *)(sequence + 28);
			for (int e = 0; e < events; ++e)
			{
				uint8_t *event = sequence + eventIndex + e * eventSize;
				const char *options = (const char *)event + 12;
				if ((*(const int32_t *)(event + 8) & kNewStyleEvent) || (!strstr(options, "Pump") && !strstr(options, "Bolt")))
					continue;

				*(int32_t *)(event + 4) = on ? kPlaySoundEvent : 0;
				++found;
			}
		}
		return found;
	}

	// Attachments: index by name, and their place on a bone (the bone and the matrix) inside the model
	const int kAttachmentPlacementSize = 52;

	inline int FindAttachment(const void *header, const char *name)
	{
		const uint8_t *base = (const uint8_t *)header;
		const int32_t count = *(const int32_t *)(base + 240), index = *(const int32_t *)(base + 244);
		for (int i = 0; i < count; ++i)
		{
			const uint8_t *attachment = base + index + i * 92;
			if (_stricmp((const char *)attachment + *(const int32_t *)attachment, name) == 0)
				return i;
		}
		return 0;
	}

	inline void *AttachmentPlacement(const void *header, int attachment)
	{
		const uint8_t *base = (const uint8_t *)header;
		return (uint8_t *)base + *(const int32_t *)(base + 244) + attachment * 92 + 8;
	}

	// The bone the attachment `name` is on and its place on that bone, -1 if the model has none by that name
	inline int AttachmentOnBone(const void *header, const char *name, HandMath::Mat34 &onBone)
	{
		const uint8_t *base = (const uint8_t *)header;
		const int32_t count = *(const int32_t *)(base + 240), index = *(const int32_t *)(base + 244);
		for (int i = 0; i < count; ++i)
		{
			const uint8_t *attachment = base + index + i * 92;
			if (_stricmp((const char *)attachment + *(const int32_t *)attachment, name) == 0)
			{
				memcpy(&onBone, attachment + 12, sizeof(float) * 12);
				return *(const int32_t *)(attachment + 8);
			}
		}
		return -1;
	}

	const int kMaxBones = 128;

	// The model's hitboxes (its first set) together, as a box in the frame of the bone `inBone` as the model was made.
	// The weapons' world models have one around all their vertices. False if it has none.
	inline bool HitboxBounds(const void *header, int inBone, HandMath::Vec3 &boxMin, HandMath::Vec3 &boxMax)
	{
		const uint8_t *base = (const uint8_t *)header;
		const int32_t sets = *(const int32_t *)(base + 172), setIndex = *(const int32_t *)(base + 176);
		const int bones = BoneCount(header);
		if (sets <= 0 || inBone < 0 || inBone >= bones)
			return false;

		const uint8_t *set = base + setIndex;
		const int32_t count = *(const int32_t *)(set + 4), index = *(const int32_t *)(set + 8);
		bool found = false;
		for (int i = 0; i < count && i < 64; ++i)
		{
			const uint8_t *box = set + index + i * 68;
			const int32_t bone = *(const int32_t *)box;
			if (bone < 0 || bone >= bones)
				continue;
			float corners[6];
			memcpy(corners, box + 8, sizeof(corners));

			// From the hitbox's bone to the model, then to the other bone
			const HandMath::Mat34 toBone = HandMath::Multiply(PoseToBone(header, inBone), HandMath::InvertRigid(PoseToBone(header, bone)));
			for (int corner = 0; corner < 8; ++corner)
			{
				const HandMath::Vec3 point = HandMath::TransformPoint(toBone,
					HandMath::Vec3(corners[(corner & 1) ? 3 : 0], corners[(corner & 2) ? 4 : 1], corners[(corner & 4) ? 5 : 2]));
				if (!found)
				{
					boxMin = boxMax = point;
					found = true;
				}
				boxMin = HandMath::Vec3(fminf(boxMin.x, point.x), fminf(boxMin.y, point.y), fminf(boxMin.z, point.z));
				boxMax = HandMath::Vec3(fmaxf(boxMax.x, point.x), fmaxf(boxMax.y, point.y), fmaxf(boxMax.z, point.z));
			}
		}
		return found;
	}
}
