#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "studio.h"
#include "pose_relay.h"
#include "diagnostics.h"

using namespace HandMath;

// What the player carries is seen on the body:
// - the ammo bag at the waist, where magazines and shells are taken (manual reload),
// - pills or adrenaline on the chest by the left shoulder, a grenade on the right shoulder; grabbing one takes it.
// They're the game's own models, drawn with the weapon in first person (DrawBodyModels). Other VR players with the mod
// are seen with theirs at the same places, where the game draws what each player carries.

namespace
{
	// The client's data tables, to find where a networked variable is in an entity (Source engine layout)
	struct RecvTable;
	struct RecvProp
	{
		const char *name;
		int type;
		int flags;
		int stringBufferSize;
		int insideArray;
		const void *extraData;
		RecvProp *arrayProp;
		void *arrayLengthProxy;
		void *proxy;
		void *dataTableProxy;
		RecvTable *dataTable;
		int offset;
		int elementStride;
		int elements;
		const char *parentArrayName;
	};
	struct RecvTable
	{
		RecvProp *props;
		int count;
		void *decoder;
		const char *name;
	};
	struct ClientClass
	{
		void *create;
		void *createEvent;
		const char *networkName;
		RecvTable *table;
		ClientClass *next;
		int classId;
	};
	static_assert(sizeof(RecvProp) == 0x3C, "RecvProp of the Source engine");

	bool FindProp(const RecvTable *table, const char *name, int base, int depth, int &offset, int &count)
	{
		if (!table || !table->props || depth > 10)
			return false;

		for (int i = 0; i < table->count; ++i)
		{
			const RecvProp &prop = table->props[i];
			if (!prop.name)
				continue;
			if (strcmp(prop.name, name) == 0)
			{
				offset = base + prop.offset;
				count = prop.dataTable ? prop.dataTable->count : 1;
				return true;
			}
			if (prop.dataTable && FindProp(prop.dataTable, name, base + prop.offset, depth + 1, offset, count))
				return true;
		}
		return false;
	}

	// A wrong layout would read garbage: it's caught instead of crashing
	bool FindPlayerProp(void *firstClass, const char *name, int &offset, int &count)
	{
		__try
		{
			for (const ClientClass *clientClass = (const ClientClass *)firstClass; clientClass; clientClass = clientClass->next)
			{
				if (clientClass->networkName && strcmp(clientClass->networkName, "CTerrorPlayer") == 0)
					return FindProp(clientClass->table, name, 0, 0, offset, count);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		return false;
	}

	const int kClientAmmoArrayOffset = 0xF24; // known, checks the lookup
	int g_MyWeaponsOffset = -1;
	int g_MyWeaponsCount = 0;
	bool g_NetvarsLookedUp = false;

	bool HasPlayerClass(const ClientClass *clientClass)
	{
		for (int i = 0; clientClass && i < 1000; ++i, clientClass = clientClass->next)
		{
			if (clientClass->networkName && strcmp(clientClass->networkName, "CTerrorPlayer") == 0)
				return true;
		}
		return false;
	}

	// IBaseClientDLL::GetAllClasses, found by its code (mov eax, [g_pClientClassHead]; ret) instead of trusting its place
	// in the interface, which changed between engine versions. The list it gives has to have the player's class.
	void *GetAllClasses(void *baseClientDll, int &foundIndex)
	{
		foundIndex = -1;
		__try
		{
			void **vtable = *(void ***)baseClientDll;
			for (int index = 0; index <= 40; ++index)
			{
				const uint8_t *code = (const uint8_t *)vtable[index];
				if (!code || code[0] != 0xA1 || code[5] != 0xC3)
					continue;
				void *classes = **(void ***)(code + 1);
				if (HasPlayerClass((const ClientClass *)classes))
				{
					foundIndex = index;
					return classes;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		return nullptr;
	}

	void LookUpNetvars(Game *game)
	{
		g_NetvarsLookedUp = true;
		int classesIndex = -1;
		void *classes = game->m_BaseClientDll ? GetAllClasses(game->m_BaseClientDll, classesIndex) : nullptr;
		Diagnostics::Log("body items: the client's classes %s (IBaseClientDLL function %d)", classes ? "found" : "not found", classesIndex);

		int ammoOffset = -1, ammoCount = 0, weaponsOffset = -1, weaponsCount = 0;
		const bool foundAmmo = classes && FindPlayerProp(classes, "m_iAmmo", ammoOffset, ammoCount);
		const bool foundWeapons = classes && FindPlayerProp(classes, "m_hMyWeapons", weaponsOffset, weaponsCount);

		if (foundAmmo && ammoOffset == kClientAmmoArrayOffset && foundWeapons && weaponsCount > 0 && weaponsCount <= 64)
		{
			g_MyWeaponsOffset = weaponsOffset;
			g_MyWeaponsCount = weaponsCount;
			Diagnostics::Log("body items: m_hMyWeapons at 0x%X, %d of them", weaponsOffset, weaponsCount);
		}
		else
		{
			Diagnostics::Log("body items: the inventory wasn't found (m_iAmmo %s at 0x%X, m_hMyWeapons %s at 0x%X, %d), nothing is shown on the chest",
				foundAmmo ? "found" : "not found", ammoOffset, foundWeapons ? "found" : "not found", weaponsOffset, weaponsCount);
		}
	}

	struct BodyModel
	{
		const char *path;
		const void *model;
		bool lookedUp;
	};

	// The bag is the game's ammo pack. Only models the game has loaded can be drawn.
	BodyModel g_Models[] =
	{
		{ "models/w_models/weapons/w_eq_explosive_ammopack.mdl", nullptr, false },
		{ "models/w_models/weapons/w_eq_incendiary_ammopack.mdl", nullptr, false },
		{ "models/w_models/weapons/w_eq_painpills.mdl", nullptr, false },
		{ "models/w_models/weapons/w_eq_adrenaline.mdl", nullptr, false },
		{ "models/w_models/weapons/w_eq_molotov.mdl", nullptr, false },
		{ "models/w_models/weapons/w_eq_pipebomb.mdl", nullptr, false },
		{ "models/w_models/weapons/w_eq_bile_flask.mdl", nullptr, false },
	};
	enum { Model_Bag, Model_BagOther, Model_Pills, Model_Adrenaline, Model_Molotov, Model_PipeBomb, Model_BileJar };

	const void *GetModel(Game *game, int which)
	{
		BodyModel &entry = g_Models[which];
		if (!entry.lookedUp)
		{
			entry.lookedUp = true;
			const int index = game->m_ModelInfo->GetModelIndex(entry.path);
			entry.model = index >= 0 ? game->m_ModelInfo->GetModel(index) : nullptr;
			Diagnostics::Log("body items: %s %s", entry.path, entry.model ? "loaded" : "not loaded by the game");
		}
		return entry.model;
	}

	int ModelOfItem(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::PAIN_PILLS: return Model_Pills;
		case C_WeaponCSBase::ADRENALINE: return Model_Adrenaline;
		case C_WeaponCSBase::MOLOTOV: return Model_Molotov;
		case C_WeaponCSBase::PIPE_BOMB: return Model_PipeBomb;
		case C_WeaponCSBase::VOMITJAR: return Model_BileJar;
		default: return -1;
		}
	}

	// Which place on the chest an item goes to: 0 pills or adrenaline, 1 grenade, -1 none
	int ChestSpotOf(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::PAIN_PILLS:
		case C_WeaponCSBase::ADRENALINE:
			return 0;
		case C_WeaponCSBase::MOLOTOV:
		case C_WeaponCSBase::PIPE_BOMB:
		case C_WeaponCSBase::VOMITJAR:
			return 1;
		default:
			return -1;
		}
	}

	// Meters from the eyes looking straight ahead, for a right handed player of 1.8 m: forward, to the left, up.
	// Pills by the left shoulder, the grenade on the right one.
	const Vec3 kChestSpots[2] = { Vec3(0.16f, 0.13f, -0.27f), Vec3(0.15f, -0.16f, -0.24f) };
	const float kChestGrabRadius = 0.12f; // meters
	const float kChestViewCosine = 0.2f;

	// The place the body items are measured from: where the eyes would be with the head looking straight ahead (the
	// head turns around the neck), facing where the head faces horizontally. x forward, y left, z up.
	Mat34 BodyFrame(const Vec3 &head, const Vec3 &headForward, const Vec3 &headUp, float unitsPerMeter)
	{
		Vec3 facing(headForward.x, headForward.y, 0.0f);
		if (Length(facing) < 0.2f)
			facing = Vec3(headForward.x * headUp.z - headUp.x * headForward.z, headForward.y * headUp.z - headUp.y * headForward.z, 0.0f);
		facing = Normalized(facing);

		const float neckForward = 0.08f * unitsPerMeter, neckDown = 0.075f * unitsPerMeter;
		const Vec3 neck = head - headForward * neckForward - headUp * neckDown;
		const Vec3 reference = neck + facing * neckForward + Vec3(0.0f, 0.0f, neckDown);
		const Vec3 up(0.0f, 0.0f, 1.0f);
		return FromAxes(facing, Cross(up, facing), up, reference);
	}

	// An item standing upright on the chest, facing forward. The adrenaline's model lies along its y axis.
	Mat34 ChestItemFrame(const Mat34 &body, int spot, int weaponId, bool leftHanded, float playerHeight, float unitsPerMeter)
	{
		Vec3 offset = kChestSpots[spot];
		offset.z *= playerHeight / 1.8f;
		if (leftHanded)
			offset.y = -offset.y;

		const Vec3 point = TransformPoint(body, offset * unitsPerMeter);
		const Vec3 forward = Axis(body, 0), up(0.0f, 0.0f, 1.0f);
		if (weaponId == C_WeaponCSBase::ADRENALINE)
			return FromAxes(forward, up, Cross(forward, up), point);
		return FromAxes(forward, Cross(up, forward), up, point);
	}

	template <typename T>
	T Read(const void *entity, int offset)
	{
		return *(const T *)((const uint8_t *)entity + offset);
	}

	// The entity index in a handle (NUM_ENT_ENTRY_BITS of the engine)
	const int kHandleIndexMask = 0xFFF;

	bool IsValidHandle(int handle)
	{
		return handle != 0 && handle != -1;
	}

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }
}

void VR::ClearBodyModels()
{
	for (BodyModel &entry : g_Models)
	{
		entry.model = nullptr;
		entry.lookedUp = false;
	}
	m_BodyDrawCount = 0;
	for (bool &valid : m_RemoteHeadOffsetValid)
		valid = false;
}

// Every frame, after the tracking: what is drawn on the body and where
void VR::UpdateBodyItems()
{
	m_BodyDrawCount = 0;
	m_AmmoBagDrawn = false;
	m_ChestItemIds[0] = m_ChestItemIds[1] = 0;

	if (!m_Game->m_EngineClient->IsInGame())
		return;

	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local)
		return;

	// The bag
	if (m_ManualReloadEnabled)
	{
		const void *bag = GetModel(m_Game, Model_Bag);
		if (!bag)
			bag = GetModel(m_Game, Model_BagOther);
		if (bag)
		{
			m_AmmoBagDrawn = true;
			if (m_ReloadPouchVisible)
			{
				// Against the hip: its thin side faces out
				const Vec3 up(0.0f, 0.0f, 1.0f);
				const Vec3 forward = Normalized(m_WaistForward);
				const Vec3 outward = Cross(up, forward) * (m_LeftHanded ? -1.0f : 1.0f);
				BodyModelDraw &draw = m_BodyDraws[m_BodyDrawCount++];
				draw.model = bag;
				draw.frame = FromAxes(outward, Cross(up, outward), up, m_WaistWorld);
			}
		}
	}

	// Pills, adrenaline and grenades carried, not in the hand
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	if (g_MyWeaponsOffset < 0)
		return;

	const void *active = local->GetActiveWeapon();
	for (int i = 0; i < g_MyWeaponsCount; ++i)
	{
		const int handle = Read<int>(local, g_MyWeaponsOffset + i * 4);
		if (!IsValidHandle(handle))
			continue;
		C_WeaponCSBase *weapon = (C_WeaponCSBase *)m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
		if (!weapon || weapon == active)
			continue;
		const int weaponId = weapon->GetWeaponID();
		const int spot = ChestSpotOf(weaponId);
		if (spot >= 0)
			m_ChestItemIds[spot] = weaponId;
	}

	const Mat34 body = BodyFrame(ToVec3(m_HmdPosAbs), ToVec3(m_HmdForward), ToVec3(m_HmdUp), m_VRScale);
	for (int spot = 0; spot < 2; ++spot)
	{
		const int weaponId = m_ChestItemIds[spot];
		if (!weaponId)
			continue;
		const Mat34 frame = ChestItemFrame(body, spot, weaponId, m_LeftHanded, m_PlayerHeightInMeters, m_VRScale);
		m_ChestItemPoints[spot] = Origin(frame);

		const void *model = GetModel(m_Game, ModelOfItem(weaponId));
		const bool inView = Dot(Normalized(Origin(frame) - ToVec3(m_HmdPosAbs)), ToVec3(m_HmdForward)) > kChestViewCosine;
		if (model && inView && m_BodyDrawCount < kMaxBodyDraws)
		{
			BodyModelDraw &draw = m_BodyDraws[m_BodyDrawCount++];
			draw.model = model;
			draw.frame = frame;
		}
	}
}

// Called after each draw of the weapon in first person, so they're drawn in the same pass as it
void VR::DrawBodyModels(int flags, void *renderable, int entityIndex)
{
	for (int i = 0; i < m_BodyDrawCount; ++i)
		Hooks::DrawBodyModel(flags, renderable, entityIndex, m_BodyDraws[i].model, m_BodyDraws[i].frame);
}

// The slot of the item on the chest where the hand is (slot3 grenade, slot5 pills or adrenaline), 0 if none
int VR::GetChestItemSlotAt(const Vec3 &handWorld) const
{
	for (int spot = 0; spot < 2; ++spot)
	{
		if (m_ChestItemIds[spot] && Length(handWorld - m_ChestItemPoints[spot]) < kChestGrabRadius * m_VRScale)
			return spot == 0 ? 5 : 3;
	}
	return 0;
}

// The bones of an item drawn on the body: the model as it was made, its center at `frame`
void VR::BuildBodyModelBones(const void *studioHeader, const Mat34 &frame, void *outBones)
{
	Mat34 *bones = (Mat34 *)outBones;
	const int boneCount = std::clamp(Studio::BoneCount(studioHeader), 0, Studio::kMaxBones);

	const Vec3 hullMin = *(const Vec3 *)((const uint8_t *)studioHeader + 104);
	const Vec3 hullMax = *(const Vec3 *)((const uint8_t *)studioHeader + 116);
	Mat34 placed = frame;
	SetAxis(placed, 3, TransformPoint(frame, (hullMin + hullMax) * -0.5f));

	for (int i = 0; i < boneCount; ++i)
		bones[i] = Multiply(placed, InvertRigid(Studio::PoseToBone(studioHeader, i)));
}

// Another VR player's pills or grenade, drawn by the game where that player carries them: moved to the chest. False
// when it isn't one.
bool VR::BuildRemoteItemBones(int entityIndex, const void *weapon, const void *studioHeader, void *outBones)
{
	if (g_MyWeaponsOffset < 0)
		return false;

	// Only an entity found in a player's inventory is known to be a weapon: weapons lying around in the map are drawn
	// with the same models and aren't
	for (int player = 1; player <= 32; ++player)
	{
		if (!m_RemoteHeadOffsetValid[player])
			continue;

		C_BasePlayer *owner = (C_BasePlayer *)m_Game->GetClientEntity(player);
		if (!owner || (const void *)owner->GetActiveWeapon() == weapon)
			continue;

		bool carries = false;
		for (int i = 0; i < g_MyWeaponsCount && !carries; ++i)
		{
			const int handle = Read<int>(owner, g_MyWeaponsOffset + i * 4);
			carries = IsValidHandle(handle) && (handle & kHandleIndexMask) == entityIndex;
		}
		if (!carries)
			continue;

		const int weaponId = ((C_WeaponCSBase *)weapon)->GetWeaponID();
		const int spot = ChestSpotOf(weaponId);
		if (spot < 0)
			return false;

		PoseRelay::Frame pose;
		if (!m_Game->m_PoseRelay->GetPose(player, pose))
			return false;

		const Mat34 body = BodyFrame(pose.head.position + m_RemoteHeadOffset[player], pose.head.forward, pose.head.up, m_VRScale);
		BuildBodyModelBones(studioHeader, ChestItemFrame(body, spot, weaponId, false, 1.8f, m_VRScale), outBones);
		return true;
	}
	return false;
}
