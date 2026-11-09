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

    Offset ServerFireTerrorBullets =     { "server.dll", 0x3C78B0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
    Offset ReadUserCmd =                 { "server.dll", 0x205710, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
    // CBaseCSGrenade::StartGrenadeThrow, called when the trigger is let go: m_fThrowTime = curtime + 0.2
    Offset StartGrenadeThrow =           { "server.dll", 0x24E230, "55 8B EC 51 A1 ? ? ? ? F3 0F 10 40 0C F3 0F 58 05 ? ? ? ? 56 8B F1 F3 0F 10 8E ? ? ? ? 0F 2E C8" };
    // CBaseCSGrenade::ThrowGrenade, called when m_fThrowTime is reached. Calls the virtual EmitGrenade(origin, angles, velocity, angular impulse, owner)
    Offset ThrowGrenade =                { "server.dll", 0x24E2A0, "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 57 8B F9 E8 ? ? ? ? 8B F0 85 F6 0F 84 ? ? ? ? 8B 06 8B 90 68 01 00 00 8B CE FF D2 84 C0" };
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