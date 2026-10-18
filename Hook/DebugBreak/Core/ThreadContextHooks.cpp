#include "DebugBreakInternal.h"

BOOL
WINAPI
NewSetThreadContext(
    _In_ HANDLE hThread,
    _In_ CONTEXT* lpContext
)
{
    if (!lpContext)
    {
        return FALSE;
    }

    if (DebugBreakInternal::ShouldBypassThreadContextHooks())
    {
        return Sys_SetThreadContext(hThread, lpContext);
    }

    bool hardwareBreakpointsSynced = true;
    if (DebugBreakInternal::HasContextDebugRegisterRequest(lpContext->ContextFlags))
    {
        // x64dbg/TitanEngine 的部分 bph 路径只写 DR0-DR3 地址，不置 DR7 的 Ln 位，
        // 实测 SetThreadContext 收到的 DR7 固定为 0x400（仅 bit10），断点永不触发。
        // 这里按“地址非 0 即视为启用”补齐本地使能位，与调试器意图一致。
        for (int drIndex = 0; drIndex < DebugBreakInternal::kDebugRegisterCount; ++drIndex)
        {
            if (DebugBreakInternal::ReadDebugRegister(lpContext, drIndex) != 0 &&
                !DebugBreakInternal::IsDebugRegisterEnabled(lpContext->Dr7, drIndex))
            {
                lpContext->Dr7 |= (1ull << (drIndex * 2));
            }
        }

        logger.Log("[DR] SetThreadContext tid=%u flags=0x%lX dr0=0x%llX dr1=0x%llX dr2=0x%llX dr3=0x%llX dr6=0x%llX dr7=0x%llX",
            GetThreadId(hThread),
            lpContext->ContextFlags,
            lpContext->Dr0,
            lpContext->Dr1,
            lpContext->Dr2,
            lpContext->Dr3,
            lpContext->Dr6,
            lpContext->Dr7);

        if (!DebugBreakInternal::IsHardwareBreakpointBridgeEnabled())
        {
            if (DebugBreakInternal::HasEnabledDebugRegister(lpContext))
            {
                DebugBreakInternal::ClearLocalHardwareBreakpointRecords(true, "硬件断点请求被拒绝");
                logger.Log("已拒绝硬件断点请求: errorCode=1005, tid=%u, dr7=0x%llX, 请使用软件INT3断点",
                    GetThreadId(hThread),
                    lpContext->Dr7);
                SetLastError(ERROR_NOT_SUPPORTED);
                return FALSE;
            }

            DebugBreakInternal::ClearLocalHardwareBreakpointRecords(false, "硬件断点桥关闭");
        }
        // *rch = (*rch << 4) + (*(szHex + i) - 'A' + 10);
        else
        {
            if (!DebugBreakInternal::IsCurrentHardwareBreakpointState(lpContext))
            {
                hardwareBreakpointsSynced = DebugBreakInternal::SyncHardwareBreakpointsFromContext(lpContext);
            }
        }

        //可能是为了清理dr6
        if (lpContext->Dr6 == 0)
        {
            InterlockedExchange(&g_debug_condition_detected, 0);
            InterlockedExchange(&g_debug_register_status, 0);
            DebugBreakInternal::ClearDebugHitRecord(GetThreadId(hThread));
        }

        if (!hardwareBreakpointsSynced)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
    }

    const DWORD contextFlagsWithoutDebug =
        DebugBreakInternal::StripContextDebugRegisterRequest(lpContext->ContextFlags);
    if (!DebugBreakInternal::HasNonDebugContextRequest(contextFlagsWithoutDebug))
    {
        return TRUE;
    }

    CONTEXT contextToSet = *lpContext;
    contextToSet.ContextFlags = contextFlagsWithoutDebug;
    return Sys_SetThreadContext(hThread, &contextToSet);
}

namespace
{
    constexpr NTSTATUS kStatusSuccess = 0;
    constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000Du);
    constexpr NTSTATUS kStatusNotSupported = static_cast<NTSTATUS>(0xC00000BBu);
}

// ntdll!NtSetContextThread 代理。
//
// 为什么必须单独拦截 ntdll 层：
//   kernel32!SetThreadContext 只是 ntdll!NtSetContextThread 的薄封装，调试器
//   （x64dbg/TitanEngine、CE）设置硬件断点时经常直接调用 ntdll 导出以避开 Win32 层。
//   只 hook kernel32!SetThreadContext 时，bph 请求会静默丢失：调试器侧断点列表显示成功，
//   VT 侧从未收到 IOCTL_SET_HARDWARE_BREAKPOINT，目标执行时既不命中也不报错。
//   这里复用 NewSetThreadContext 的桥接逻辑，保证两条调用路径行为一致。
NTSTATUS NTAPI NewNtSetContextThread(
    _In_ HANDLE hThread,
    _In_ PCONTEXT lpContext
)
{
    if (!lpContext)
    {
        return Sys_NtSetContextThread(hThread, lpContext);
    }

    if (DebugBreakInternal::ShouldBypassThreadContextHooks())
    {
        return Sys_NtSetContextThread(hThread, lpContext);
    }

    if (!DebugBreakInternal::HasContextDebugRegisterRequest(lpContext->ContextFlags))
    {
        return Sys_NtSetContextThread(hThread, lpContext);
    }

    CONTEXT context = *lpContext;

    // 与 NewSetThreadContext 一致：补齐 x64dbg 未写入的 DR7 使能位，否则断点永不触发。
    for (int drIndex = 0; drIndex < DebugBreakInternal::kDebugRegisterCount; ++drIndex)
    {
        if (DebugBreakInternal::ReadDebugRegister(&context, drIndex) != 0 &&
            !DebugBreakInternal::IsDebugRegisterEnabled(context.Dr7, drIndex))
        {
            context.Dr7 |= (1ull << (drIndex * 2));
        }
    }

    // 诊断：bph 排查期间确认调试器的 DR 请求确实到达 ntdll 层。
    logger.Log("[DR] NtSetContextThread tid=%u flags=0x%lX dr0=0x%llX dr1=0x%llX dr2=0x%llX dr3=0x%llX dr6=0x%llX dr7=0x%llX",
        GetThreadId(hThread),
        context.ContextFlags,
        context.Dr0,
        context.Dr1,
        context.Dr2,
        context.Dr3,
        context.Dr6,
        context.Dr7);

    bool hardwareBreakpointsSynced = true;

    if (!DebugBreakInternal::IsHardwareBreakpointBridgeEnabled())
    {
        if (DebugBreakInternal::HasEnabledDebugRegister(&context))
        {
            DebugBreakInternal::ClearLocalHardwareBreakpointRecords(true, "硬件断点请求被拒绝");
            logger.Log("已拒绝硬件断点请求(NtSetContextThread): tid=%u, dr7=0x%llX, 请使用软件INT3断点",
                GetThreadId(hThread),
                context.Dr7);
            return kStatusNotSupported;
        }

        DebugBreakInternal::ClearLocalHardwareBreakpointRecords(false, "硬件断点桥关闭");
    }
    else if (!DebugBreakInternal::IsCurrentHardwareBreakpointState(&context))
    {
        hardwareBreakpointsSynced = DebugBreakInternal::SyncHardwareBreakpointsFromContext(&context);
    }

    // 与 NewSetThreadContext 保持一致：Dr6==0 视为调试器清理命中状态。
    if (context.Dr6 == 0)
    {
        InterlockedExchange(&g_debug_condition_detected, 0);
        InterlockedExchange(&g_debug_register_status, 0);
        DebugBreakInternal::ClearDebugHitRecord(GetThreadId(hThread));
    }

    if (!hardwareBreakpointsSynced)
    {
        return kStatusInvalidParameter;
    }

    const DWORD contextFlagsWithoutDebug =
        DebugBreakInternal::StripContextDebugRegisterRequest(context.ContextFlags);
    if (!DebugBreakInternal::HasNonDebugContextRequest(contextFlagsWithoutDebug))
    {
        return kStatusSuccess;
    }

    context.ContextFlags = contextFlagsWithoutDebug;
    return Sys_NtSetContextThread(hThread, &context);
}

BOOL WINAPI NewGetThreadContext(
    _In_ HANDLE hThread,
    _Inout_ LPCONTEXT lpContext
)
{
    if (!lpContext)
    {
        return FALSE;
    }

    if (DebugBreakInternal::ShouldBypassThreadContextHooks())
    {
        return Sys_GetThreadContext(hThread, lpContext);
    }

    const DWORD requestedContextFlags = lpContext->ContextFlags;
    //调用原函数
    BOOL boSuccess = Sys_GetThreadContext(hThread, lpContext);

    if (boSuccess && DebugBreakInternal::HasContextDebugRegisterRequest(requestedContextFlags))
    {
        const bool hardwareBridgeEnabled = DebugBreakInternal::IsHardwareBreakpointBridgeEnabled();
        if (!hardwareBridgeEnabled)
        {
            DebugBreakInternal::ClearLocalHardwareBreakpointRecords(false, "硬件断点桥关闭");
        }

        lpContext->Dr0 = 0;
        lpContext->Dr1 = 0;
        lpContext->Dr2 = 0;
        lpContext->Dr3 = 0;
        lpContext->Dr6 = 0;
        lpContext->Dr7 = 0x400;

        const DWORD threadId = GetThreadId(hThread);
        DEBUG_HIT_RECORD hitRecord = { 0 };
        const bool hasThreadHitRecord = DebugBreakInternal::GetDebugHitRecord(threadId, &hitRecord);
        const bool useGlobalHitRecord = !hasThreadHitRecord && threadId == 0;
        LONG debugCondition = hasThreadHitRecord ?
            hitRecord.Condition :
            (useGlobalHitRecord ? InterlockedCompareExchange(&g_debug_condition_detected, 0, 0) : 0);
        const LONG rawDebugRegisterStatus = (hasThreadHitRecord ?
            hitRecord.RegisterStatus :
            (useGlobalHitRecord ? InterlockedCompareExchange(&g_debug_register_status, 0, 0) : 0)) & 0xF;
        LONG debugRegisterStatus = hardwareBridgeEnabled ? rawDebugRegisterStatus : 0;
        bool hasCapturedDr6Status = debugRegisterStatus != 0;
        ULONG64 detectedBreakpointAddress = hasThreadHitRecord ? hitRecord.DetectedAddress : 0;

        const LONG activeHardwareBreakpointCount =
            InterlockedCompareExchange(&g_active_hardware_breakpoint_count, 0, 0);
        if (hardwareBridgeEnabled &&
            activeHardwareBreakpointCount > 0 &&
            !hasCapturedDr6Status &&
            detectedBreakpointAddress == 0)
        {
            // 该路径只窥探一次性命中状态，真实事件仍由 WaitForDebugEvent 消费。
            int detectedDebugRegister = -1;
            detectedBreakpointAddress = DebugBreakInternal::QueryDetectedBreakpointAddress(
                hThread,
                &detectedDebugRegister);
            if (detectedBreakpointAddress != 0)
            {
                debugCondition = 1;
                if (detectedDebugRegister >= 0 &&
                    detectedDebugRegister < DebugBreakInternal::kDebugRegisterCount)
                {
                    debugRegisterStatus = 1L << detectedDebugRegister;
                    hasCapturedDr6Status = true;
                }
                DebugBreakInternal::UpdateDebugHitDetectedAddress(
                    threadId,
                    debugCondition,
                    debugRegisterStatus,
                    detectedBreakpointAddress);
            }
        }

        bool reportedDetectedBreakpoint = false;
        int fallbackDebugRegister = -1;
        int validBreakpointCount = 0;

        BreakpointList.Lock();
        for (const auto& Breakpoint : BreakpointList) //即使如果列表为0，它也会自动跳过，不会进for循环里
        {
            if (Breakpoint.Address &&
                Breakpoint.debugRegister >= 0 &&
                Breakpoint.debugRegister < DebugBreakInternal::kDebugRegisterCount)
            {
                DebugBreakInternal::WriteDebugRegister(lpContext, Breakpoint.debugRegister, Breakpoint.Address);
                lpContext->Dr7 |= (1ull << (Breakpoint.debugRegister * 2));
                lpContext->Dr7 |= (static_cast<ULONG64>(Breakpoint.debugRw & 0x3) <<
                    (16 + Breakpoint.debugRegister * 4));
                lpContext->Dr7 |= (static_cast<ULONG64>(Breakpoint.debugLen & 0x3) <<
                    (18 + Breakpoint.debugRegister * 4));
                fallbackDebugRegister = Breakpoint.debugRegister;
                ++validBreakpointCount;

                if (debugCondition != 0 &&
                    !hasCapturedDr6Status &&
                    detectedBreakpointAddress != 0 &&
                    DebugBreakInternal::IsDetectedAddressInBreakpointRange(Breakpoint, detectedBreakpointAddress))
                {
                    lpContext->Dr6 |= (1ull << Breakpoint.debugRegister);
                    reportedDetectedBreakpoint = true;
                }
            }
        }
        BreakpointList.UnLock();

        if (hardwareBridgeEnabled && debugCondition == 1 && hasCapturedDr6Status)
        {
            lpContext->Dr6 |= debugRegisterStatus;
        }
        else if (hardwareBridgeEnabled &&
            debugCondition == 1 &&
            !hasCapturedDr6Status &&
            detectedBreakpointAddress == 0 &&
            validBreakpointCount == 1 &&
            fallbackDebugRegister >= 0)
        {
            lpContext->Dr6 |= (1ull << fallbackDebugRegister);
        }
        else if (debugCondition == 2 && !reportedDetectedBreakpoint)
        {
            lpContext->Dr6 = 1 << 14; //设置bs位
        }

        // 旧项目保留的禁用路径：GetThreadContext 时通过 vmcall2(VMCALL_GET_BREAKPOINT) 查询当前线程命中的地址，
        // 再和回填到 lpContext 的 DR0-DR3 做相等比较以设置 DR6。
        // 为什么注释：重构后命中信息优先由驱动侧 DEBUG_HIT_RECORD 记录，并支持寄存器状态位、地址范围和单步 BS 位；
        // 旧路径只比较起始地址，不能覆盖 LEN>1 的监视范围，也缺少线程命中缓存。
        // 放开的好处：能直接复现旧版 VT 查询行为，便于定位 vmcall2 返回值是否正确。
        // 放开的坏处：会覆盖当前 DR6 合成结果，遇到范围断点、寄存器状态已捕获或单步异常时可能给调试器错误状态。
        // 项目影响：调试器依赖 DR6 判断哪个硬件断点命中，放开会影响断点继续执行、异常分发和反调试兼容性。
        // 结论：继续注释；需要调试时可以短期开启日志或 QueryDetectedBreakpointAddress，不建议恢复旧分支。
        //if (BreakpointList.size() > 0)
        //{
        //    DWORD dwThreadId = GetThreadId(hThread);
        //    if (dwThreadId == 0)
        //    {
        //        ReportSeriousError("无法获取线程id！");
        //        return boSuccess;
        //    }
        //
        //    BREAKPOINT_DETECTED vmcallinfo = { 0 };
        //    vmcallinfo.command = VMCALL_GET_BREAKPOINT;
        //    vmcallinfo.Cid.UniqueThread = (HANDLE)dwThreadId;
        //
        //    if (vmcall2(&vmcallinfo))
        //    {
        //        logger.Log("breakpoint_detected: %p", vmcallinfo.breakpoint_detected);
        //        if (vmcallinfo.breakpoint_detected == lpContext->Dr0)
        //        {
        //            lpContext->Dr6 = 1 << 0;
        //        }
        //        else if (vmcallinfo.breakpoint_detected == lpContext->Dr1)
        //        {
        //            lpContext->Dr6 = 1 << 1;
        //        }
        //        else if (vmcallinfo.breakpoint_detected == lpContext->Dr2)
        //        {
        //            lpContext->Dr6 = 1 << 2;
        //        }
        //        else if (vmcallinfo.breakpoint_detected == lpContext->Dr3)
        //        {
        //            lpContext->Dr6 = 1 << 3;
        //        }
        //        logger.Log("lpContext->Dr6: %p", lpContext->Dr6);
        //    }
        //}
    }
    //    logger.Log("current_vmcall 失败!  errorCode:%d", vmcallinfo.errorCode);
    //SYSTEM_INFO SysInfo = { 0 };
    //GetSystemInfo(&SysInfo);
    //vmcallinfo.CPUCount = SysInfo.dwNumberOfProcessors;
    //boSuccess = current_vmcall(&vmcallinfo);
    //if (boSuccess)
    //{
    //    logger.Log("返回的监视id: %d", vmcallinfo.watchid);
    //    // Add the breakpoint
    //    BREAKPOINT_RECORD Breakpoint = { 0 };
    //    Breakpoint.Address = vmcallinfo.VirtualAddress;
    //    Breakpoint.length = vmcallinfo.Size;
    //    Breakpoint.cr3 = vmcallinfo.cr3;
    //    Breakpoint.watchid = vmcallinfo.watchid;
    //    BreakpointList.Lock();
    //    BreakpointList.push_back(Breakpoint);
    //    BreakpointList.UnLock();
    //}
    //else
    return boSuccess;
}
