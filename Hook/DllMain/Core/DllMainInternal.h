#pragma once

#include "../../dllmain.h"
#include "../../HookCallSet/functionSet.h"
#include "../../Channels/DispatchData.h"
#include "../../Globals.h"
#include "../../Log/Log.h"
#include <new>
#ifndef UNREALVTDBG_NO_VMP_SDK_IMPORT
#define UNREALVTDBG_NO_VMP_SDK_IMPORT
#endif
#include "../../../Common/VMProtect/vmp.h"

namespace HookDllMainInternal
{
    extern volatile LONG g_HookInstalled;
    extern volatile LONG g_HookRuntimeStarted;
    extern volatile LONG g_HookRuntimeStopping;
    extern volatile LONG g_HookRuntimeCompleted;
    extern HINSTANCE g_HookModule;
    extern HMODULE g_HookRuntimeModuleReference;
    extern HANDLE g_HookRuntimeThreadHandle;
    extern DWORD g_HookRuntimeThreadId;

    void EarlyHookLog(const wchar_t* message);
    // 启动期文件 trace：不依赖 CRT/logger，用于定位 DllMain/runtime 卡死点。
    void BootTrace(const char* stage);
    void InitializeHookLogger(HINSTANCE moduleHandle);
    bool VerifyDriverHandshake();
    DWORD WINAPI HookRuntimeThread(LPVOID parameter);
    bool StartHookRuntime(HINSTANCE moduleHandle);
    void RequestHookRuntimeShutdown();
    void CloseHookRuntimeThreadHandle();
}
