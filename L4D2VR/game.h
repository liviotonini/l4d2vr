#pragma once
#include <cstdint>
#include <array>
#include <chrono>
#include "vector.h"

class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IMaterialSystem;
class IBaseClientDLL;
class IViewRender;
class IViewRender;
class CBaseEntity;
class IModelInfo;
class IModelRender;
class IMaterial;
class IInput;
class ISurface;
class C_BasePlayer;
struct model_t;

class Game;
class Offsets;
class VR;
class Hooks;
class PoseRelay;

inline Game *g_Game;

struct Player
{
    C_BasePlayer *pPlayer;
    bool isUsingVR;
    QAngle controllerAngle;
    Vector controllerPos;
    bool isMeleeing;
    bool isNewSwing;
    QAngle prevControllerAngle;

    bool manualReload = false;  // reloads with the hands: the game doesn't reload this player's weapon by itself
    bool reloadButton = false;  // has the reload button pressed in the last usercmd
    bool onePistolDoubled = false; // holds two pistols shown as one, which does double damage

    // Where this player's hand aims the use for a moment, after grabbing something in the world
    Vector useAimOrigin = { 0,0,0 };
    QAngle useAimAngles = { 0,0,0 };
    unsigned long long useAimUntil = 0;
    bool useAimLeft = false;    // grabbed with the left hand: what it takes goes to the left hand

    // Dual wield (dual_wield.cpp). What the left hand holds, as the player says, and the melee weapon carried beside a
    // pistol. Both are checked after every batch of usercmds: the entity has to belong to this player.
    int leftItemIndex = 0;
    void *leftItem = nullptr;
    int leftItemId = 0;
    int extraMeleeIndex = 0;
    void *extraMelee = nullptr;
    // The left hand, sent while it swings a melee weapon or uses its item
    Vector leftHandPos = { 0,0,0 };
    QAngle leftHandAngle = { 0,0,0 };
    QAngle leftSwingPrevAngle = { 0,0,0 };
    unsigned long long leftHandUntil = 0;   // the pose is recent
    unsigned long long leftSwingUntil = 0;  // swinging
    bool leftSwingNew = true;
    bool leftSwingTraced = true;            // the last pose received has been used

    // Last grenade throw received from the player (VR::EncodeGrenadeThrow, decoded in Hooks::dReadUsercmd)
    int throwSequence = 0;
    bool throwPending = false;  // not thrown yet
    bool throwGesture = false;  // thrown with the arm
    Vector throwArmVelocity = { 0,0,0 };
    std::chrono::steady_clock::time_point throwTime; // when it was received

    Player()
        : isUsingVR(false),
        controllerAngle({ 0,0,0 }),
        controllerPos({ 0,0,0 }),
        isMeleeing(false),
        isNewSwing(false),
        prevControllerAngle({ 0,0,0 })
    {}
};

class Game 
{
public:
    IClientEntityList *m_ClientEntityList = nullptr;
    IEngineTrace *m_EngineTrace = nullptr;
    IEngineClient *m_EngineClient = nullptr;
    IMaterialSystem *m_MaterialSystem = nullptr;
    IBaseClientDLL *m_BaseClientDll = nullptr;
    IViewRender *m_ClientViewRender = nullptr;
    IViewRender *m_EngineViewRender = nullptr;
    IModelInfo *m_ModelInfo = nullptr;
    IModelRender *m_ModelRender = nullptr;
    IInput *m_VguiInput = nullptr;
    ISurface *m_VguiSurface = nullptr;

    uintptr_t m_BaseEngine;
    uintptr_t m_BaseClient;
    uintptr_t m_BaseServer;
    uintptr_t m_BaseMaterialSystem;
    uintptr_t m_BaseVgui2;

    Offsets *m_Offsets = nullptr;
    VR *m_VR = nullptr;
    Hooks *m_Hooks = nullptr;
    PoseRelay *m_PoseRelay = nullptr;

    bool m_Initialized = false;

    // Indexed by the entity index of the player. The game has up to 32 players, more with server mods.
    std::array<Player, 65> m_PlayersVRInfo;
    int m_CurrentUsercmdID = -1;
    bool IsValidPlayerIndex(int index) const { return index >= 0 && index < (int)m_PlayersVRInfo.size(); }
    bool m_PerformingMelee = false;
    const Vector *m_MeleeEyeOverride = nullptr; // the hand the melee traces start from, when not the right one

    bool m_IsMeleeWeaponActive = false;
    bool m_SwitchedWeapons = false;

    Game();

    void *GetInterface(const char *dllname, const char *interfacename);

    static void errorMsg(const char *msg);

    CBaseEntity *GetClientEntity(int entityIndex);
    char *getNetworkName(uintptr_t *entity);
    void ClientCmd(const char *szCmdString);
    void ClientCmd_Unrestricted(const char *szCmdString);

    typedef void *(__cdecl *tCreateInterface)(const char *name, int *returnCode);
};

