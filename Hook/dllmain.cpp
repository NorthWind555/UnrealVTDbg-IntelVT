#include "DllMain/Core/DllMainInternal.h"

BOOL WINAPI DllMain(
    HINSTANCE hinstDLL,  // handle to DLL module
    DWORD fdwReason,     // reason for calling function
    LPVOID lpReserved)   // reserved
{
    BOOL bRet = TRUE;

    // Perform actions based on the reason for calling.
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
    {
        HookDllMainInternal::BootTrace("DllMain attach enter");
        DisableThreadLibraryCalls(hinstDLL);
        if (!HookDllMainInternal::StartHookRuntime(hinstDLL))
        {
            HookDllMainInternal::BootTrace("StartHookRuntime failed");
            bRet = FALSE;
        }
        HookDllMainInternal::BootTrace("DllMain attach leave");
        break;
    }

    case DLL_THREAD_ATTACH:
        // Do thread-specific initialization.
        break;

    case DLL_THREAD_DETACH:
    {
        // Do thread-specific cleanup.
        break;
    }

    case DLL_PROCESS_DETACH:
    {
        HookDllMainInternal::RequestHookRuntimeShutdown();
        break;
    }

    }
    return bRet;
}
