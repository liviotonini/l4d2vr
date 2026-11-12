// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <iostream>
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include "sdk.h"
#include "diagnostics.h"
#include "stability.h"
#include "vr_enabled.h"
#include <cstdio>
#include <cstring>

bool L4D2VR_IsEnabled()
{
    static const bool enabled = []
    {
        FILE *file = nullptr;
        if (fopen_s(&file, "VR\\config.txt", "r") != 0 || !file)
            return true;
        bool value = true;
        char line[256];
        while (fgets(line, sizeof(line), file))
        {
            const char kKey[] = "VREnabled=";
            if (strncmp(line, kKey, sizeof(kKey) - 1) == 0)
            {
                const char *setting = line + sizeof(kKey) - 1;
                value = !(_strnicmp(setting, "false", 5) == 0 || setting[0] == '0');
            }
        }
        fclose(file);
        return value;
    }();
    return enabled;
}

DWORD WINAPI InitL4D2VR(HMODULE hModule)
{
    // VREnabled=false: the game as without the mod (the d3d9.dll calls go to Windows' own, d3d9_main.cpp)
    if (!L4D2VR_IsEnabled())
        return 0;

#ifdef _DEBUG
    AllocConsole();
    FILE *fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif

    // Make sure -insecure is used
    LPWSTR *szArglist;
    int nArgs;
    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    bool insecureEnabled = false;
    for (int i = 0; i < nArgs; ++i)
    {
        if (wcscmp(szArglist[i], L"-insecure") == 0)
            insecureEnabled = true;
    }
    LocalFree(szArglist);

    if (!insecureEnabled)
        ExitProcess(0);

    Diagnostics::Init();
    Diagnostics::Log("L4D2VR starting");
    Stability::Init();

    g_Game = new Game();

    return 0;
}



BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
            CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitL4D2VR, hModule, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}


