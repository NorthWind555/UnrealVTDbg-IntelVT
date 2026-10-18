#include "GlobalsInternal.h"

namespace hv
{
    void inject_single_step(__vcpu* vcpu)
    {
        bool const kernelMode = get_guest_cpl() == 0;
        if (!kernelMode)
        {
            PCLIENT_ID cid = GuestCurrentThreadCid();
            if (cid != nullptr)
            {
                vcpu->Cid.UniqueThread = cid->UniqueThread;
            }
            else
            {
                // ETHREAD.Cid 偏移未就绪（InitNtoskrnl 初始化为 InvalidOffset，
                // 只能由 VMCALL_INIT_OFFSET 修正）。Cid 仅用于把命中归属到具体线程，
                // 缺失时只应退化为"不记录线程"，不能因此吞掉 #DB —— 否则 EPT watch
                // 命中后调试器永远收不到调试事件，硬件/数据断点表现为完全不生效。
                vcpu->Cid = { 0 };
            }

            // Intel event injection 不会替 guest 更新 DR6；快照必须与 #DB 一起投递。
            unsigned __int64 debugRegisterState = vcpu->vmexit_info.guest_registers->dr6;
            debugRegisterState &= ~((1ULL << 14) | 0xFULL);
            if (vcpu->breakpoint_debug_register >= 0 &&
                vcpu->breakpoint_debug_register < 4)
            {
                debugRegisterState |= 1ULL << vcpu->breakpoint_debug_register;
            }
            inject_interruption_with_debug_state(
                EXCEPTION_VECTOR_SINGLE_STEP,
                INTERRUPT_TYPE_HARDWARE_EXCEPTION,
                0,
                false,
                debugRegisterState);
            return;
        }

        vcpu->Cid = { 0 };
        vcpu->breakpoint_detected = NULL;
        vcpu->breakpoint_debug_register = -1;
    }

    // 调用方必须持有 eptWatchList_lock；本函数只做表内查找，不自行加锁。
    int getIdleWatchID()
    {
        for (int i = 0; i < EPTWATCHLISTSIZE; i++)
        {
            if (!ept_watch_entry_is_reserved(eptWatchList[i]))
            {
                return i;
            }
        }

        return -1;
    }
}
