#pragma once
#include "sigscanner.h"
#include "game.h"
#include "diagnostics.h"


struct Offset
{
    std::string moduleName;
    int offset;
    int address = 0; // stays 0 if the signature isn't found
    std::string signature;
    int sigOffset;

    // An optional offset only turns off its feature when the game changes: no error at startup, `address` stays 0
    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0, bool optional = false)
    {
        this->moduleName = moduleName;
        this->offset = currentOffset;
        this->signature = signature;
        this->sigOffset = sigOffset;

        int newOffset = SigScanner::VerifyOffset(moduleName, currentOffset, signature, sigOffset);
        if (newOffset > 0)
        {
            this->offset = newOffset;
        }
            
        if (newOffset == -1)
        {
            Diagnostics::Log("signature not found in %s: %s", moduleName.c_str(), signature.c_str());
            if (!optional)
                Game::errorMsg(("Signature not found: " + signature).c_str());
            return;
        }

        this->address = (uintptr_t)GetModuleHandle(moduleName.c_str()) + this->offset;
    }
};

class Offsets
{
public:
    Offset RenderView =                  { "client.dll", 0x1D6D80, "55 8B EC 81 EC ? ? ? ? 53 56 57 8B D9" };
    Offset g_pClientMode =               { "client.dll", 0x228551, "89 04 B5 ? ? ? ? E8", 3 };
    Offset CalcViewModelView =           { "client.dll", 0x288F40, "55 8B EC 83 EC 48 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 10" };
    Offset ClientFireTerrorBullets =     { "client.dll", 0x2F6090, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10"};
    Offset WriteUsercmdDeltaToBuffer =   { "client.dll", 0x134850, "55 8B EC 83 EC 60 0F 57 C0 8B 55 0C" };
    Offset WriteUsercmd =                { "client.dll", 0x1AADD0, "55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 10 56 57" };
    Offset g_pppInput =                  { "client.dll", 0xA8A62, "8B 0D ? ? ? ? 8B 01 8B 50 58 FF E2", 2 };
    Offset AdjustEngineViewport =        { "client.dll", 0x31C5C0, "55 8B EC 8B 0D ? ? ? ? 85 C9 74 17" };
    Offset TestMeleeSwingClient =        { "client.dll", 0x30DD60, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B 75 08 57 8B D9 E8 ? ? ? ? 8B" };
    Offset GetMeleeWeaponInfoClient =    { "client.dll", 0x30D290, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset IsSplitScreen =               { "client.dll", 0x1B2AD0, "33 C0 83 3D ? ? ? ? ? 0F 9D C0" };
    Offset PrePushRenderTarget =         { "client.dll", 0xA8CB0, "55 8B EC 8B C1 56 8B 75 08 8B 0E 89 08 8B 56 04 89" };
    // Operand of "mov ecx, [r_flashlight_attach_to_viewmodel.m_pParent]" inside C_TerrorPlayer::Simulate
    Offset FlashlightAttachToViewmodel = { "client.dll", 0x260774, "8B 0D ? ? ? ? 83 79 30 00 0F 84 ? ? ? ? 6A 00 8B CF E8", 2 };
    // "lea eax, [ebp-0x1F0]" (viewmodel attachment angles passed to AngleVectors) inside the first person laser sight update of C_TerrorPlayer
    Offset LaserSightAttachmentAngles =  { "client.dll", 0x25E143, "8D 85 F8 FD FF FF 50 8D 8D E0 FD FF FF 51 8D 95 FC FE FF FF 52 8D 85 10 FE FF FF 50 E8", 0x15 };
    // C_BasePlayer::GetViewModel(int index), resolves m_hViewModel[index]
    Offset GetViewModel =                { "client.dll", 0x67330, "55 8B EC 8B 45 08 53 57 8B F9 8B 84 87 ? ? ? ? 83 F8 FF 74 ? 8B 15 ? ? ? ? 8B C8 81 E1 FF 0F 00 00" };
    // FormatViewModelAttachment(C_BasePlayer *player, Vector &origin, bool inverse), converts points between the viewmodel FOV and the world FOV
    Offset FormatViewModelAttachment =   { "client.dll", 0x6A8B0, "55 8B EC 8B 45 08 83 EC 28 53 56 57 33 FF 85 C0 74 ? 50 E8 ? ? ? ? 83 C4 04 83 F8 FF 75 ? 8B 0D ? ? ? ? 8B 01 8B 90 F8 01 00 00 FF D2 8B F8 8B 0D ? ? ? ? 8B 01 8B 50 30 57 FF D2 8B F0 D9 46 34" };

    // Interacting with the hand: what the game looks at to find the entity to use, and the eyes of the player it looks from
    Offset ClientPlayerEyeVectors =      { "client.dll", 0x20450, "55 8B EC 56 8B F1 8B 86 ? ? ? ? 83 F8 FF 74 5B", 0, true };
    Offset ClientPlayerEyePosition =     { "client.dll", 0x20EB0, "55 8B EC 56 8B F1 8B 86 ? ? ? ? 83 F8 FF 74 5D", 0, true };
    Offset ClientFindUseEntity =         { "client.dll", 0x283200, "55 8B EC 81 EC 78 0B 00 00 A1 ? ? ? ? 33 C5 89 45 FC 0F 57 C9 F3 0F 10 45 0C 0F 2F C8 F3 0F 10 55 08", 0, true };
    Offset ServerPlayerEyeAngles =       { "server.dll", 0x7B0F0, "55 8B EC 83 EC 64 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 64 80 3D ? ? ? ? 00 75 5B 8B 87 ? ? ? ? 83 F8 FF 74 50", 0, true };
    Offset ServerPlayerEyePosition =     { "server.dll", 0x7B2A0, "55 8B EC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 71 80 3D ? ? ? ? 00 75 68 8B 87 ? ? ? ? 83 F8 FF 74 5D", 0, true };
    Offset FindUseEntity =               { "server.dll", 0x34E790, "55 8B EC B8 80 13 00 00 E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 0F 57 C9 F3 0F 10 45 0C 0F 2F C8 F3 0F 10 55 08", 0, true };
    Offset PlayerUse =                   { "server.dll", 0x312BB0, "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B F1 8B 9E B8 1C 00 00 8B 06 8B 90 28 01 00 00", 0, true };

    // Where a reload starts (DefaultReload for weapons with a magazine, Reload for the shotguns), and the state of the
    // "helping hand" animation that reaches towards things to use
    Offset ClientDefaultReload =         { "client.dll", 0x17F10, "55 8B EC 51 56 8B F1 8B 86 ? ? ? ? 57 83 F8 FF 74 36 8B 15", 0, true };
    Offset ClientShotgunReload =         { "client.dll", 0x2F1CF0, "55 8B EC 51 56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? A8 01 0F 85 ? ? ? ? A9 00 08 00 00", 0, true };
    Offset ClientSetHelpingHandState =   { "client.dll", 0x2FC5E0, "55 8B EC 51 53 56 8B F1 8D 9E ? ? ? ? 57 8B 7D 08 89 5D FC 39 BE ? ? ? ? 74 06 89 BE", 0, true };
    Offset ServerDefaultReload =         { "server.dll", 0x4DD90, "55 8B EC 51 53 56 8B F1 8B 86 ? ? ? ? 83 F8 FF 74 36 8B 15", 0, true };
    Offset ServerShotgunReload =         { "server.dll", 0x3C2F80, "55 8B EC 83 EC 08 56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? A8 01 0F 85 ? ? ? ? A9 00 08 00 00", 0, true };
    Offset ServerSetHelpingHandState =   { "server.dll", 0x3D0A30, "55 8B EC 51 53 8B 5D 08 56 8B F1 57 8D BE ? ? ? ? 89 7D FC 39 9E ? ? ? ? 74 2A", 0, true };

    // CBaseCombatWeapon::GetSlot, the bucket of the weapon's script: a melee weapon in the left hand gets one of its own
    Offset ClientGetSlot =               { "client.dll", 0x15390, "0F B7 81 ? ? ? ? 50 E8 ? ? ? ? 8B 80 58 01 00 00 83 C4 04 C3", 0, true };
    Offset ServerGetSlot =               { "server.dll", 0x4A960, "0F B7 81 ? ? ? ? 50 E8 ? ? ? ? 8B 80 58 01 00 00 83 C4 04 C3", 0, true };

    // The call of CGrenadeLauncher_Projectile::Create(origin, angles, velocity, angular velocity, owner, flags) in the
    // grenade launcher's fire: the grenade leaves from the eyes the game's way, the hook takes it to the weapon
    Offset GrenadeLauncherCreateCall =   { "server.dll", 0x3CF420, "52 8B 90 34 02 00 00 8B CE F3 0F 11 45 90 F3 0F 11 45 94 F3 0F 11 45 98 FF D2 50 8D 45 AC 50 E8", 31, true };

    // The end of the use of pills and adrenaline, in their frame: owner->IsIncapacitated(), owner->IsAlive(),
    // this->ApplyEffect(owner), the owner's buttons restored, owner->Weapon_Drop(this, 0, 0), UTIL_Remove(this). The
    // offsets of those virtuals are read from it (DualWield::ServerUseItem).
    Offset ItemUseFinish =               { "server.dll", 0x3C26E5, "8B 17 8B 82 ? ? ? ? 8B CF FF D0 84 C0 75 ? 8B 17 8B 82 ? ? ? ? 8B CF FF D0 84 C0 74 ? 8B 16 8B 82 ? ? ? ? 57 8B CE FF D0 8B 4D FC 8B 17 8B 82 ? ? ? ? 6A 00 6A 00 89 8F ? ? ? ? 56 8B CF FF D0 56 E8", 0, true };

    // CBasePlayer::m_pCurrentCommand, read by CLagCompensationManager::StartLagCompensation, which goes on with it null
    // after a warning: a shot fired outside the player's usercmds is given one (LeftGun::ServerFire). Points at the offset.
    Offset ServerCurrentCommand =        { "server.dll", 0x187EDC, "8B 9F ? ? ? ? 89 5D EC 85 DB 75 0E 68", 2, true };
    // The player's weapon of a weapon ID (the weapons' virtual 0x634 is GetWeaponID): a pistol taken, from the ground or
    // from a weapon spawn, looks there for the pistol to make a pair with (Hooks::dServerWeaponOfId)
    Offset ServerWeaponOfId =            { "server.dll", 0x242B20, "55 8B EC 53 56 57 33 FF 8D 99 ? ? ? ? 8B FF 8B 03 83 F8 FF 74 ? 8B 15 ? ? ? ? 8B C8 81 E1 FF 0F 00 00 03 C9 8D 4C CA 04 85 C9 74 ? C1 E8 0C 39 41 04 75 ? 8B 31 85 F6 74 ? 8B 06 8B 90 ? ? ? ? 8B CE FF D2 39 45 08 74", 0, true };
    // CTerrorPlayer's weapon of a weapon ID from its table by ID: where a weapon spawn looks for the pistol to make a pair
    // with (Hooks::dServerWeaponOfId)
    Offset ServerWeaponById =            { "server.dll", 0x2C98F0, "55 8B EC 8B 45 08 0F B6 84 08 ? ? ? ? 85 C0 74 ? 8B 84 81 ? ? ? ? 83 F8 FF 74 ? 8B 15 ? ? ? ? 8B C8 81 E1 FF 0F 00 00 03 C9 8D 4C CA 04 85 C9 74 ? C1 E8 0C 39 41 04 75 ? 8B 01 5D C2 04 00", 0, true };
    // UTIL_ParticleTracer(name, start, end, entity, attachment, whiz): the bullets' tracers, from where the weapon in the
    // right hand is. The left hand's gun's start from its own (Hooks::dClientParticleTracer).
    Offset ClientParticleTracer =        { "client.dll", 0x1AD200, "55 8B EC 8B 45 08 50 E8 ? ? ? ? D9 EE 8B 4D 1C 8B 55 18 83 C4 04 50 8B 45 14 68 ? ? ? ? 51 51 8B 4D 10 D9 1C 24 52 8B 55 0C 50 51 52 E8", 0, true };
    // CTerrorGun's virtual that makes a pair of the pistol a player has when they take another one (m_hasDualWeapons):
    // VR players hold the second one in the other hand instead (Hooks::dServerAddDualWeapon)
    Offset ServerAddDualWeapon =         { "server.dll", 0x3E8350, "55 8B EC 83 EC 20 56 8B F1 80 BE ? ? ? ? 00 74 07 32 C0 5E 8B E5 5D C3 80 BE ? ? ? ? 00 75 F0 80 BE ? ? ? ? 01", 0, true };
    // CBasePlayer::SelectItem(weapon): the checks of a weapon change, then Weapon_Switch. Takes out one weapon entity,
    // where "use <class>" takes the first one of two of the same kind (DualWield::ServerSelectWeapon).
    Offset ServerSelectItem =            { "server.dll", 0x7A960, "55 8B EC 56 8B F1 8B 06 8B 90 ? ? ? ? FF D2 85 C0 75 ? 8B 06 8B 90 ? ? ? ? 57 8B 7D 08 57 8B CE FF D2 84 C0 74 ? 8B CE E8 ? ? ? ? 85 C0 74 ? 8B CE E8 ? ? ? ? 8B 10 8B C8 8B 82 ? ? ? ? FF D0 84 C0 74 ? 8B CE E8 ? ? ? ? 8B 16 8B 82 ? ? ? ? 6A 00 57 8B CE FF D0 5F 5E 5D C2 04 00", 0, true };

    Offset ServerFireTerrorBullets =     { "server.dll", 0x3C78B0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
    Offset ReadUserCmd =                 { "server.dll", 0x205710, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
    // CBaseCSGrenade::StartGrenadeThrow, called when the trigger is let go: m_fThrowTime = curtime + 0.2
    Offset StartGrenadeThrow =           { "server.dll", 0x24E230, "55 8B EC 51 A1 ? ? ? ? F3 0F 10 40 0C F3 0F 58 05 ? ? ? ? 56 8B F1 F3 0F 10 8E ? ? ? ? 0F 2E C8" };
    // CBaseCSGrenade::ThrowGrenade, called when m_fThrowTime is reached. Calls the virtual EmitGrenade(origin, angles, velocity, angular impulse, owner)
    Offset ThrowGrenade =                { "server.dll", 0x24E2A0, "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 57 8B F9 E8 ? ? ? ? 8B F0 85 F6 0F 84 ? ? ? ? 8B 06 8B 90 68 01 00 00 8B CE FF D2 84 C0" };
    // CBaseCSGrenade::ItemPostFrame once m_fThrowTime is reached: owner->RemoveAmmo(1, m_iPrimaryAmmoType) (virtual, its
    // offset at +10, the ammo type's at +2), then ThrowGrenade. Without that the grenade stays with the player.
    // CTerrorPlayer::AddUpgrade(type): the upgrade goes on the primary weapon (Weapon_GetSlot(0), virtual, its offset at
    // +0x32) through the weapon's AddUpgrade (virtual, its offset at +0xBF), then "Player.AwardUpgrade" (+0xCB) with
    // EmitSound (call at +0xD1). Hooks::dPlayerAddUpgrade puts a laser sight on the gun of the hand that took it.
    Offset ServerPlayerAddUpgrade =      { "server.dll", 0x308EE0, "55 8B EC 81 EC 18 01 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 8B 5D 08 56 57 53 8B F9 E8 ? ? ? ? 83 C4 04 89 85 EC FE FF FF 85 C0 74 76 8B 07 8B 90", 0, true };
    // In C_TerrorPlayer's laser sight update: ParticleProp()->Create("weapon_laser_sight_FP", PATTACH_WORLDORIGIN, ...),
    // the first person laser. The left hand's gun's is made the same way (VR::UpdateLeftLaser).
    Offset ClientLaserSightCreate =      { "client.dll", 0x25DFB3, "6A 05 8D 8E ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? 8D 8C 7F 38 0A 00 00 8B 54 8E 08", 0, true };
    Offset GrenadeThrowTimeReached =     { "server.dll", 0x24E153, "8B 86 ? ? ? ? 8B 13 8B 92 ? ? ? ? 50 6A 01 8B CB FF D2 8B 06 8B 90 ? ? ? ? 8B CE FF D2", 0, true };
    Offset ProcessUsercmds =             { "server.dll", 0xEFB20, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };
    Offset CBaseEntity_entindex =        { "server.dll", 0x253A0, "8B 41 28 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC CC CC CC CC CC CC CC CC CC CC 55"};
    Offset TestMeleeSwingServer =        { "server.dll", 0x3EB2E0, "24 FF D2 5B 5F 5E C3", 20};
    Offset DoMeleeSwingServer =          { "server.dll", 0x3EBDC0, "55 8B EC 83 EC 3C 53 56 8B F1 E8 ? ? ? ? 8B D8 85" };
    Offset StartMeleeSwingServer =       { "server.dll", 0x3EC080, "55 8B EC 53 56 8B F1 8B 86 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? 8B" };
    Offset PrimaryAttackServer =         { "server.dll", 0x3EC3B0, "56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? 83 F8 FF" };
    Offset ItemPostFrameServer =         { "server.dll", 0x3EC4A0, "56 57 8B F1 E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 53" };
    Offset GetPrimaryAttackActivity =    { "server.dll", 0x3EAF30, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?" };
    Offset GetActiveWeapon =             { "server.dll", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64 };
    Offset GetMeleeWeaponInfo =          { "server.dll", 0x3EA0D0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset EyePosition =                 { "server.dll", 0x6D980, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" };

    Offset GetRenderTarget =             { "materialsystem.dll", 0x2CD90, "83 79 4C 00" };
    Offset Viewport =                    { "materialsystem.dll", 0x2E070, "55 8B EC 83 EC 28 8B C1" };
    Offset GetViewport =                 { "materialsystem.dll", 0x2D2A0, "55 8B EC 8B 41 4C 8B 49 40 8D 04 C0 83 7C 81 ? ?" };
    Offset PushRenderTargetAndViewport = { "materialsystem.dll", 0x2D650, "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89" };
    Offset PopRenderTargetAndViewport =  { "materialsystem.dll", 0x2CEE0, "56 8B F1 83 7E 4C 00" };

    Offset DrawModelExecute =            { "engine.dll", 0xE0600, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B" };
    Offset VGui_Paint =                  { "engine.dll", 0x115D10, "55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38" };
};