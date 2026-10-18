# 20 硬件断点（DRx）设计说明 + 本次「DR 透传」改动评审

评审对象（分支 `fix/hw-bp-dr-passthrough`，相对 `main`）：
- `VT_Driver/VmExit/VmExitDebugRegisters.cpp`（写物理 DR 后同步 guest 快照）
- `VT_Driver/Vmcs/VmcsControls.cpp`（`mov_dr_exiting` 恒为 true）

---

## 一、为什么 docs/00 写「不含：硬件断点（DRx）」

**结论：是设计取舍，不是没来及做。**「不含 DRx」指的是**不以物理调试寄存器为实现手段**，而不是「代码完全不碰 DR」。

1. 项目的硬件断点能力走的是 **EPT Watch**（EPT violation 驱动）方案。
   - `README.md`：*「硬件断点虚拟化：通过 EPT Violation 事件模拟硬件断点，突破 DR0-DR7 数量限制（最多支持 512 个监控点），支持读/写/执行三种访问类型监控」*。
   - 即：靠 EPT 页权限拦截访问，命中后**软件模拟 DR6 命中状态 + 注入 #DB**，让来宾调试器（CE / x64dbg）以为命中了硬件断点。这样能突破物理 DR 只有 4 个槽位的限制。
   - 证据：`Ept/Watch/EptWatchEvents.cpp:280` 记 `breakpoint_debug_register`；`Globals/GuestWatchState.cpp` 用 `dr6` 快照置位并注入 `#DB`；`HypervisorRoutines/HypervisorInterrupts.cpp:65` 直接改 `guest_registers->dr6`。

2. 但「不用 DRx 做断点」≠「不碰 DR」。为了**让来宾对 DR 的访问透明**（来宾内核/调试器会读写 DR），驱动必须手动保存/恢复 DR0–DR3/DR6：
   - `ASM/vm-exit.asm:49-59`：VM-exit 入口把物理 DR0-3/DR6 存进栈上 `guest_context` 快照。
   - `ASM/vm-exit.asm:120-130`：VMRESUME 前用快照恢复物理 DR0-3/DR6。
   - 原因：x86 **只把 DR7 做成 VMCS 字段（`GUEST_DR7`）**，DR0-DR3/DR6 **不是** VMCS 字段，硬件不会在 host/guest 之间自动切换，只能由汇编手动存取。

3. **坑点（docs/09 P1-4）**：`DR7.GD=1` 时，**任何**访问 DR 的指令都先触发 `#DB`（SDM 17.2.1）；而 `vm-exit.asm` 的出口是**无条件** `mov dr0..dr6`。root 运行期间物理 DR7 = 来宾 DR7（无 host 侧 DR7 自动切换），来宾一旦置 GD，root 执行 `mov dr0,rax` 立即 `#DB`，落到 `Tables/idt.cpp:98-104` 的**强制三重故障**路径。

   所以「尽量不依赖 DRx」是有意规避这条危险路径；只要还做 DR 透明化，这个坑就一直挂着（P1-4）。

---

## 二、本次新增代码评审

### 改动 A：`VmExitDebugRegisters.cpp` —— 写物理 DR 后同步 `guest_registers` 快照

判定：**正确且必要。**

- `vm-exit.asm` 的出口恢复用的是**栈上 `guest_context` 快照**；该快照在每个 VM-exit 周期开头（`sub rsp,1C0h` 之后）保存，**不跨周期持久化**。
- 因此 root handler 若只写物理 DR、不同步快照，下一轮 VMRESUME 的出口恢复就会用**入口时的旧快照**覆盖刚写入的新值 → 来宾新设的硬件断点被抹掉。
- 改动对 DR0/DR1/DR2/DR3/DR6 逐一同步快照，与 asm 的保存/恢复字段一一对应。✓
- DR7 不在快照范围内（asm 不碰 DR7），走 `vmwrite(GUEST_DR7)`，故 `case 7` 无需同步快照 —— **与 asm 一致，正确**。✓
- 附带补 UTF-8 BOM：消除 `C4819`，✓。

### 改动 B：`VmcsControls.cpp` —— `mov_dr_exiting` 恒为 true

判定：**方向正确，且恰好修掉一处死代码。**

- 坐实前提：`_MINIMAL` 在 `Vmcs/VmcsFields.h:7` 与 `vmcs.h:21` **无条件 `#define`**（docs/09 P0-2）。
- 因此原 `#ifdef _MINIMAL` 恒取 **false** 分支 → 非精简的 `mov_dr_exiting=true` 是**死代码** → `vmexit_mov_dr_handler` 实际**从不被触发**。本次改成恒 true，等于**把沉寂的 MOV DR 处理重新接上**。✓
- 代价：MOV DR 现在恒 VM-exit。MOV DR 是极低频指令（仅调试态），性能影响可忽略。
- 风格遗留：同文件其它 `#ifdef _MINIMAL`（invlpg / rdtsc / io_bitmaps / wbinvd）未动；那些是性能取舍，可保持。

### 本次改动**未覆盖**的相关风险（评审重点）

| 级别 | 问题 | 位置 | 说明 |
| --- | --- | --- | --- |
| **高** | **DR7.GD 未处理**（P1-4） | `VmExitDebugRegisters.cpp:53`、`ASM/vm-exit.asm:120-130` | handler 写 DR7 时只清 reserved 位、**不清 `general_detect`**（`__dr7` bit13）。来宾置 GD 后：asm 下一轮出口 `mov dr0..dr6` → root `#DB` → 三重故障；handler 自身 `__writedr/__readdr` 同样踩雷。触发条件：目标/调试器设置 `DR7.GD`（经典反调试手段）；普通目标不复现。 |
| **中** | 缺 CPL 检查 | `VmExitDebugRegisters.cpp`（两条路径） | MOV DR 是特权指令，CPL>0 应注入 `#GP(0)`。当前 handler 未判 CPL，会把用户态 MOV DR 当合法执行。 |

### 正确点（无需改，供确认）

- 非法 DR 编号（4/5/8–15）→ 注入 `#UD` 且**不** `adjust_rip`：fault 语义正确。✓
- 读方向 DR7 用 `vmread(GUEST_DR7)`，与写方向 `vmwrite` 对称。✓
- 写 DR6 时把 reserved 位规范化（bits 4–11=1、bit12=0、bits17–31=1），符合 SDM。✓

---

## 三、结论与建议

1. **本次两处改动本身正确**，可以留在 `fix/hw-bp-dr-passthrough` 分支上测试主链路（常规目标）。
2. **上真机前强烈建议补 DR7.GD 防护**：一旦目标设置了 GD，会**静默三重故障**，是最难定位的一类崩溃；而对一个调试器来说，目标设 GD 是常见反调试手段，风险实际不低。
3. GD 防护的正解只有一个自洽方向：**物理 DR7 永不置 GD，GD 由软件 shadow**：
   - 记录每 vCPU 的 guest GD 影子位；
   - 来宾写 DR7 时，存进 `GUEST_DR7` 的值清 GD，物理 DR7 也清 GD；
   - 来宾后续访问 DR 时，若影子 GD=1，则由 handler 注入 `#DB`（fault 语义，不 `adjust_rip`）替代真实访问。
   - 退一步的**最小改动**：handler 一律把 GD 清掉（不允许来宾用 GD），彻底消除崩溃面，代价是不再复现 GD 的 `#DB` 语义（目标可能据此察觉，但不会崩）。
4. 测试建议：
   - 先测**普通目标**（CE/x64dbg 附加、下硬件断点/EPT watch），确认断点设置/命中/删除主链路。
   - GD 防护补完后再测「会设 `DR7.GD` 的目标」。

---

## 四、方案 A 实现记录（GD 防护，本次）

> 依据（Intel SDM §17.2.1，多方一致引用）：GD 置位时，**任何**访问调试寄存器的 MOV 指令**执行前**产生 `#DB`（**fault**），置 `DR6.BD`；且**处理器在进入 #DB 处理程序时会自动清除 GD 位**，以便处理程序访问调试寄存器。由此确定复现方式，并解决「处理程序清 GD 会不会再次 #DB」的死循环疑问。

### 改动（均在 `fix/hw-bp-dr-passthrough` 分支）

| # | 文件 | 改动 |
| --- | --- | --- |
| 1 | `Globals/GlobalsVmmContextTypes.h` | `__vcpu` 末尾新增 `unsigned __int64 guest_dr7_gd_shadow;`（附在结构末尾，不改动任何既有字段偏移） |
| 2 | `VmExit/VmExitDebugRegisters.cpp` | ① handler 开头：若 shadow GD=1 → 注入 `#DB`(fault，**不推进 RIP**)、`DR6 |= BD`、**清 shadow GD** 后返回；② 写 DR7：取 GD 存 shadow、`dr7.general_detect=0`，物理 DR7 与 `GUEST_DR7` 均清 GD；③ 读 DR7：`vmread(GUEST_DR7) \| shadow GD` |
| 3 | `Vmcs/VmcsGuest.cpp:81` | 初始化：`shadow = __readdr(7) 的 GD`，`GUEST_DR7 = __readdr(7) & ~GD` |
| 4 | `VmExit/VmExitCommon.cpp:224` | VMXOFF 恢复：`__writedr(7, guest_dr7 \| shadow GD)`（此条之后到 vmxoff/iret 不访问 DR，故置 GD 安全） |

### 关键不变量与安全性

- **物理 DR7 与 `GUEST_DR7` 恒不置 GD**。由于 `mov_dr_exiting=1`，guest 一切 MOV DR 都 VM-exit 走 handler，handler 清 GD 后才落盘 → root 期间物理 DR7.GD 恒为 0 → `vm-exit.asm` 的无条件 `mov dr0..dr6` **永不触发 #DB**，P1-4 崩溃路径被消除。唯一的例外是 VMXOFF 恢复路径末尾显式回填 GD，其后不再有任何 DR 访问。
- **#DB 语义**：GD 命中是 **fault**，故不 `adjust_rip`，指令在 guest 的 #DB 处理程序返回后重执行；此时 shadow GD 已被清除（对应硬件行为），不会无限循环。
- `guest_dr7_gd_shadow` 在 `fill_vmcs`（`Vmm/VmmProcessor.cpp:173` 每核 init）中经 `fill_vmcs_guest_fields` 初始化；`__vcpu` 走 `MmAllocateContiguousMemory`（**不清零**），故显式初始化是必需的。

### 已知偏差（低风险，记录备查）

- 报告路径 `Ept/Watch/EptWatchQuery.cpp:37` 仍直接 `vmread(GUEST_DR7)`，若 guest 置过 GD，上报的 DR7 会缺 GD 位（调试器通常不依赖 GD，暂不改）。
- `HypervisorRoutines/HypervisorVmcsDump.cpp:31` 的诊断日志同理。

### 验证

- `VT_Driver` Release x64 重建（`build_vtdrv_only.bat`：vcvars64 + `msbuild VT_Driver.vcxproj /t:Rebuild`）：见构建日志 `[BUILD EXITCODE]=0` 与 `Successfully signed`。
- 上机核对点：
  1. 常规目标下硬件断点设置/命中/删除链路正常（回归）。
  2. 用会设 `DR7.GD` 的目标验证：guest 访问 DR 时收到 `#DB`（DR6.BD=1），且**不再出现 root 三重故障蓝屏**；guest 的 #DB 处理程序可正常读 DR7。

---

## 五、实现与结论修正（2026-10-18）

> 前四节保留为历史评审记录。其中「改动 A/B」「方案 A」描述的是旧分支的 DR 透传思路；
> 主干后来换成了更完整的 MOV DR 虚拟化实现，旧方案不再落地。本节以实测为准。

### 5.1 两个提交的定位（不重复，不合并）

| 提交 | 层次 | 解决的问题 |
| --- | --- | --- |
| `88de964`（原 `35b33f7`） | VT 驱动 | `ethread_offset::Cid` 缺失时 `inject_single_step` 直接 return，EPT watch 命中后不注入 `#DB` |
| `5e5c77e`（原 `e943fb0`） | 用户态 Hook | DRx 请求没有进入桥接：DR7 无使能位、打开路径未登记 pid/cr3、`NtSetContextThread` 未拦截 |

两者作用在不同层：前者保证「命中后事件能投递」，后者保证「DR 请求能建立 EPT watch」。
代码零重叠，不应合并。

### 5.2 主干当前实现（与第二节旧描述的差异）

- `Vmcs/VmcsControls.cpp`：`mov_dr_exiting = true`（无条件）
- `VmExit/VmExitDebugRegisters.cpp`：DR0-3/DR6 写 `guest_context` 影子，DR7 用 `guest_dr7_shadow`，物理 DR 由 `vm-exit.asm` 收尾统一写回
- `Processor/drx.h`：`sanitize_guest_dr7` / `normalize_guest_dr7_shadow`，物理 DR7 与 `GUEST_DR7` 永不携带 GD
- `Vmcs/VmcsGuest.cpp`：初始化时清 GD 后写入 `GUEST_DR7`

即：不再「写物理 DR 后同步快照」，而是「只写影子、由汇编收尾落盘」，比第二节改动 A 更安全。

### 5.3 实测发现的硬断点失效根因链（x64dbg + bph）

1. **Hook 只挂 kernel32!SetThreadContext**：调试器实际从 `NtSetContextThread` 进入时请求丢失
   → 新增 `ntdll!NtSetContextThread` 拦截
2. **DR7 没有使能位**：`[DR] SetThreadContext ... dr7=0x400`（只有 bit10），DR0 有地址但 L0=0，断点永不触发
   → 桥接前按「地址非 0 即启用」补 Ln 位（`dr7=0x401`）
3. **打开（File→Open）路径不登记目标**：`NtCreateUserProcess` hook 未注册，`g_target_pid/g_target_cr3` 为 0，
   `SetBreakpoint` 报 `cr3 或 pid 为空`
   → 注册该 hook，创建进程后调用 `IOCTL_GET_PROCESS_INFO` 解析 cr3
4. **构建依赖**：默认配置链接 `VMProtectSDK64.dll`，DllMain 阶段卡死
   → 编译加 `UNREALVTDBG_NO_VMP_SDK_IMPORT`

### 5.4 已验证结果

- 目标 `AccountHelper-轻量版(需NET8运行时).exe` 入口 `RVA 0x11AD0` 下 `bph`：
  `设置硬件断点成功: watchid=0, addr=0x7FF68ADD1AD0, cr3=0x2B268F000, pid=6668, command=0x11`
- `run` 后命中，`dr6=0x1`（DR0 命中），`dr7=0x401`
- 业务路径：`ws2_32!WSASend` 断点命中，TLS ClientHello 的 SNI 为 `ww.wegame.com.cn`

### 5.5 测试注意事项（重要）

- **执行型硬断点只能下在目标进程私有代码页**（如目标 exe 自身函数）。
  下在 `ntdll/kernel32/ws2_32` 等共享 DLL 上时，EPT 执行监视按物理页清 execute 位，
  会让**所有进程**执行该页都 VM-exit；`RtlAllocateHeap` 这类高频函数会导致整机退出风暴甚至卡死。
- 打开 .NET 程序前建议关闭 x64dbg 的 TLS 回调自动断点（`x64dbg.ini` 的 `TlsCallbacks=0`），
  否则每个 DLL 加载都会中断。
- 高频函数（堆、锁、消息循环）不要设执行断点；一次性地址命中后立即 `bphc`。
