#pragma once
#include "sigscanner.h"
#include "game.h"


struct Offset
{
    std::string moduleName;
    int offset;
    int address = 0; // stays 0 if the signature isn't found
    std::string signature;
    int sigOffset;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0)
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
            Game::errorMsg(("Signature not found: " + signature).c_str());
            return;
        }

        this->address = (uintptr_t)GetModuleHandle(moduleName.c_str()) + this->offset;
    }
};

class Offsets
{
public:
    Offset RenderView =                  { "client.dll", 0x1D6C30, "55 8B EC 81 EC ? ? ? ? 53 56 57 8B D9" };
    Offset g_pClientMode =               { "client.dll", 0x228351, "89 04 B5 ? ? ? ? E8", 3 };
    Offset CalcViewModelView =           { "client.dll", 0x287270, "55 8B EC 83 EC 48 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 10" };
    Offset ClientFireTerrorBullets =     { "client.dll", 0x2F4350, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10"};
    Offset WriteUsercmdDeltaToBuffer =   { "client.dll", 0x134790, "55 8B EC 83 EC 60 0F 57 C0 8B 55 0C" };
    Offset WriteUsercmd =                { "client.dll", 0x1AAD50, "55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 10 56 57" };
    Offset g_pppInput =                  { "client.dll", 0xA8A22, "8B 0D ? ? ? ? 8B 01 8B 50 58 FF E2", 2 };
    Offset AdjustEngineViewport =        { "client.dll", 0x31A890, "55 8B EC 8B 0D ? ? ? ? 85 C9 74 17" };
    Offset TestMeleeSwingClient =        { "client.dll", 0x30C040, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B 75 08 57 8B D9 E8 ? ? ? ? 8B" };
    Offset GetMeleeWeaponInfoClient =    { "client.dll", 0x30B570, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset IsSplitScreen =               { "client.dll", 0x1B2A60, "33 C0 83 3D ? ? ? ? ? 0F 9D C0" };
    Offset PrePushRenderTarget =         { "client.dll", 0xA8C80, "55 8B EC 8B C1 56 8B 75 08 8B 0E 89 08 8B 56 04 89" };
    // Operand of "mov ecx, [r_flashlight_attach_to_viewmodel.m_pParent]" inside C_TerrorPlayer::Simulate
    Offset FlashlightAttachToViewmodel = { "client.dll", 0x260774, "8B 0D ? ? ? ? 83 79 30 00 0F 84 ? ? ? ? 6A 00 8B CF E8", 2 };
    // "lea eax, [ebp-0x1F0]" (viewmodel attachment angles passed to AngleVectors) inside the first person laser sight update of C_TerrorPlayer
    Offset LaserSightAttachmentAngles =  { "client.dll", 0x25E143, "8D 85 F8 FD FF FF 50 8D 8D E0 FD FF FF 51 8D 95 FC FE FF FF 52 8D 85 10 FE FF FF 50 E8", 0x15 };
    // C_BasePlayer::GetViewModel(int index), resolves m_hViewModel[index]
    Offset GetViewModel =                { "client.dll", 0x67330, "55 8B EC 8B 45 08 53 57 8B F9 8B 84 87 ? ? ? ? 83 F8 FF 74 ? 8B 15 ? ? ? ? 8B C8 81 E1 FF 0F 00 00" };
    // FormatViewModelAttachment(C_BasePlayer *player, Vector &origin, bool inverse), converts points between the viewmodel FOV and the world FOV
    Offset FormatViewModelAttachment =   { "client.dll", 0x6A8B0, "55 8B EC 8B 45 08 83 EC 28 53 56 57 33 FF 85 C0 74 ? 50 E8 ? ? ? ? 83 C4 04 83 F8 FF 75 ? 8B 0D ? ? ? ? 8B 01 8B 90 F8 01 00 00 FF D2 8B F8 8B 0D ? ? ? ? 8B 01 8B 50 30 57 FF D2 8B F0 D9 46 34" };

    Offset ServerFireTerrorBullets =     { "server.dll", 0x3C3FC0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
    Offset ReadUserCmd =                 { "server.dll", 0x205100, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
    // CBaseCSGrenade::StartGrenadeThrow, called when the trigger is let go: m_fThrowTime = curtime + 0.2
    Offset StartGrenadeThrow =           { "server.dll", 0x24E230, "55 8B EC 51 A1 ? ? ? ? F3 0F 10 40 0C F3 0F 58 05 ? ? ? ? 56 8B F1 F3 0F 10 8E ? ? ? ? 0F 2E C8" };
    // CBaseCSGrenade::ThrowGrenade, called when m_fThrowTime is reached. Calls the virtual EmitGrenade(origin, angles, velocity, angular impulse, owner)
    Offset ThrowGrenade =                { "server.dll", 0x24E2A0, "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 57 8B F9 E8 ? ? ? ? 8B F0 85 F6 0F 84 ? ? ? ? 8B 06 8B 90 68 01 00 00 8B CE FF D2 84 C0" };
    Offset ProcessUsercmds =             { "server.dll", 0xEF710, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };
    Offset CBaseEntity_entindex =        { "server.dll", 0x25390, "8B 41 28 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC CC CC CC CC CC CC CC CC CC CC 55"};
    Offset TestMeleeSwingServer =        { "server.dll", 0x3E79E0, "24 FF D2 5B 5F 5E C3", 20};
    Offset DoMeleeSwingServer =          { "server.dll", 0x3E84C0, "55 8B EC 83 EC 3C 53 56 8B F1 E8 ? ? ? ? 8B D8 85" };
    Offset StartMeleeSwingServer =       { "server.dll", 0x3E8780, "55 8B EC 53 56 8B F1 8B 86 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? 8B" };
    Offset PrimaryAttackServer =         { "server.dll", 0x3E8AB0, "56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? 83 F8 FF" };
    Offset ItemPostFrameServer =         { "server.dll", 0x3E8BA0, "56 57 8B F1 E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 53" };
    Offset GetPrimaryAttackActivity =    { "server.dll", 0x3E7630, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?" };
    Offset GetActiveWeapon =             { "server.dll", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64 };
    Offset GetMeleeWeaponInfo =          { "server.dll", 0x3E67D0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset EyePosition =                 { "server.dll", 0x6D610, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" };

    Offset GetRenderTarget =             { "materialsystem.dll", 0x2CD30, "83 79 4C 00" };
    Offset Viewport =                    { "materialsystem.dll", 0x2E010, "55 8B EC 83 EC 28 8B C1" };
    Offset GetViewport =                 { "materialsystem.dll", 0x2D240, "55 8B EC 8B 41 4C 8B 49 40 8D 04 C0 83 7C 81 ? ?" };
    Offset PushRenderTargetAndViewport = { "materialsystem.dll", 0x2D5F0, "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89" };
    Offset PopRenderTargetAndViewport =  { "materialsystem.dll", 0x2CE80, "56 8B F1 83 7E 4C 00" };

    Offset DrawModelExecute =            { "engine.dll", 0xE05E0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B" };
    Offset VGui_Paint =                  { "engine.dll", 0x115CE0, "55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38" };
};