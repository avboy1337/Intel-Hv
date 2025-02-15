﻿// Copyright (c) 2015-2019, Satoshi Tanda. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements VMM functions.

#include "vmm.h"
#include <intrin.h>
#include "asm.h"
#include "common.h"
#include "ept.h"
#include "log.h"
#include "util.h"
#include "performance.h"
#include "settings.h"

#define MAX_SUPPORT_PROCESS 100

//
//如果此驱动需要在vmware中测试，定义此宏
//
#define VMWARE

extern "C" {

NTSYSAPI const char *PsGetProcessImageFileName(PEPROCESS Process);


////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

// Whether VM-exit recording is enabled
static const bool kVmmpEnableRecordVmExit = false;

// How many events should be recorded per a processor
static const long kVmmpNumberOfRecords = 100;

// How many processors are supported for recording
static const long kVmmpNumberOfProcessors = 2;

////////////////////////////////////////////////////////////////////////////////
//
// types
//

// Represents raw structure of stack of VMM when VmmVmExitHandler() is called
struct VmmInitialStack {
  GpRegisters gp_regs;
  KtrapFrame trap_frame;
  ProcessorData *processor_data;
};

// Things need to be read and written by each VM-exit handler
struct GuestContext {
  union {
    VmmInitialStack *stack;
    GpRegisters *gp_regs;
  };
  FlagRegister flag_reg;
  ULONG_PTR ip;
  ULONG_PTR cr8;
  KIRQL irql;
  bool vm_continue;
};
#if defined(_AMD64_)
static_assert(sizeof(GuestContext) == 40, "Size check");
#else
static_assert(sizeof(GuestContext) == 20, "Size check");
#endif

// Context at the moment of vmexit
struct VmExitHistory {
  GpRegisters gp_regs;
  ULONG_PTR ip;
  VmExitInformation exit_reason;
  ULONG_PTR exit_qualification;
  ULONG_PTR instruction_info;
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

//
//获得本次vm-exit的进程名
//
const char* GetVmExitProcess();

bool __stdcall VmmVmExitHandler(_Inout_ VmmInitialStack *stack);

DECLSPEC_NORETURN void __stdcall VmmVmxFailureHandler(
    _Inout_ AllRegisters *all_regs);

static void VmmpHandleVmExit(_Inout_ GuestContext *guest_context);

DECLSPEC_NORETURN static void VmmpHandleTripleFault(
    _Inout_ GuestContext *guest_context);

DECLSPEC_NORETURN static void VmmpHandleUnexpectedExit(
    _Inout_ GuestContext *guest_context);

DECLSPEC_NORETURN static void VmmpHandleMonitorTrap(
    _Inout_ GuestContext *guest_context);

static void VmmpHandleException(_Inout_ GuestContext *guest_context);

static void VmmpHandleCpuid(_Inout_ GuestContext *guest_context);

static void VmmpHandleRdtsc(_Inout_ GuestContext *guest_context);

static void VmmpHandleRdtscp(_Inout_ GuestContext *guest_context);

static void VmmpHandleXsetbv(_Inout_ GuestContext *guest_context);

static void VmmpHandleMsrReadAccess(_Inout_ GuestContext *guest_context);

static void VmmpHandleMsrWriteAccess(_Inout_ GuestContext *guest_context);

static void VmmpHandleMsrAccess(_Inout_ GuestContext *guest_context,
                                _In_ bool read_access);

static void VmmpHandleGdtrOrIdtrAccess(_Inout_ GuestContext *guest_context);

static void VmmpHandleLdtrOrTrAccess(_Inout_ GuestContext *guest_context);

static void VmmpHandleDrAccess(_Inout_ GuestContext *guest_context);

static void VmmpHandleIoPort(_Inout_ GuestContext *guest_context);

static void VmmpHandleCrAccess(_Inout_ GuestContext *guest_context);

static void VmmpHandleVmx(_Inout_ GuestContext *guest_context);

static void VmmpHandleVmCall(_Inout_ GuestContext *guest_context);

static void VmmpHandleInvalidateInternalCaches(
    _Inout_ GuestContext *guest_context);

static void VmmpHandleInvalidateTlbEntry(_Inout_ GuestContext *guest_context);

static void VmmpHandleEptViolation(_Inout_ GuestContext *guest_context);

DECLSPEC_NORETURN static void VmmpHandleEptMisconfig(
    _Inout_ GuestContext *guest_context);

static ULONG_PTR *VmmpSelectRegister(_In_ ULONG index,
                                     _In_ GuestContext *guest_context);

static void VmmpDumpGuestState();

static void VmmpAdjustGuestInstructionPointer(_In_ GuestContext *guest_context);

static void VmmpIoWrapper(_In_ bool to_memory, _In_ bool is_string,
                          _In_ SIZE_T size_of_access, _In_ unsigned short port,
                          _Inout_ void *address, _In_ unsigned long count);

static void VmmpIndicateSuccessfulVmcall(_In_ GuestContext *guest_context);

static void VmmpIndicateUnsuccessfulVmcall(_In_ GuestContext *guest_context);

static void VmmpHandleVmCallTermination(_In_ GuestContext *guest_context,
                                        _Inout_ void *context);

static UCHAR VmmpGetGuestCpl();

static void VmmpInjectInterruption(_In_ InterruptionType interruption_type,
                                   _In_ InterruptionVector vector,
                                   _In_ bool deliver_error_code,
                                   _In_ ULONG32 error_code);

static ULONG_PTR VmmpGetKernelCr3();

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

// Those variables are all for diagnostic purpose
static ULONG g_vmmp_next_history_index[kVmmpNumberOfProcessors];
static VmExitHistory g_vmmp_vm_exit_history[kVmmpNumberOfProcessors]
                                           [kVmmpNumberOfRecords];

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

const char* GetVmExitProcess()
{
    return PsGetProcessImageFileName(IoGetCurrentProcess());
}



// A high level VMX handler called from AsmVmExitHandler().
// Return true for vmresume, or return false for vmxoff.
#pragma warning(push)
#pragma warning(disable : 28167)
_Use_decl_annotations_ bool __stdcall VmmVmExitHandler(VmmInitialStack *stack) {
  // Save guest's context and raise IRQL as quick as possible
  //
  //CR8是不在host state、guest state里的
  //也就是说guest_irql与guest_cr8应该是一样的
  //
  const auto guest_irql = KeGetCurrentIrql();
  const auto guest_cr8 = IsX64() ? __readcr8() : 0;

  //
  //禁止线程切换，屏蔽<=2irql的中断请求，同样不允许换页
  //这个时候很多API都是无法调用的
  //
  if (guest_irql < DISPATCH_LEVEL) {
    KeRaiseIrqlToDpcLevel();
  }

  /**
  * [确定是哪个进程vm-exit的方法（IoGetCurrentProcess）]
  * 
  * IoGetCurrentProcess的行为
  * msr[C0000101H]->gs_base->kprcb->current_thread->apc_state.process
  * 
  * Q:kprcb在system进程和其他进程的内核cr3下是同样的地址吗？
  * A:是一样的，同样的va在不同的cr3下对应的同一个物理页面，也就是说我们可以保证vm-exit前后gs指向的都是同一个物理页面
  * 然后我们可以通过IoGetCurrentProcess得到PEPROCESS，这些内核对象也应该是在不同cr3下对应相同物理页面的。那么我们
  * 就可以引用EPROCESS.ImageProcessName了。
  *
  *
  * 
  */


  //
  //打印出vm-exit的程序，在hyperplatform这种虚拟机下这样没什么问题，在正规虚拟机下比如vbox，vmware肯定不行
  //
  #if 0
  PEPROCESS Process = IoGetCurrentProcess();
  const char *ImageFileName = PsGetProcessImageFileName(Process);
  HYPERPLATFORM_LOG_INFO("vm-exit process is %s", ImageFileName);
  #endif

  // Capture the current guest state
  GuestContext guest_context = {stack,
                                UtilVmRead(VmcsField::kGuestRflags),
                                UtilVmRead(VmcsField::kGuestRip),
                                guest_cr8,
                                guest_irql,
                                true};
  guest_context.gp_regs->sp = UtilVmRead(VmcsField::kGuestRsp);

  // Update the trap frame so that Windbg can construct the stack trace of the
  // guest. The rest of trap frame fields are entirely unused. Note that until
  // this code is executed, Windbg will display incorrect stack trace based off
  // the stale, old values.
  //

 /*
 * 以下代码其实就是让dump文件方便分析guest的执行环境
 * 注意guest_state.rip有可能是guest vm-exit的rip前面、后面或者就是导致vm-exit的rip
 * 
0: kd> k
 # Child-SP          RetAddr               Call Site
00 ffffaf0f`b84b8bb0 fffff800`f9e6810b     HyperPlatform!EptHandleEptViolation+0x146
01 ffffaf0f`b84b8c50 fffff800`f9e699f7     HyperPlatform!VmmpHandleEptViolation+0x2b
02 ffffaf0f`b84b8c90 fffff800`f9e66e6e     HyperPlatform!VmmpHandleVmExit+0x1f7
03 ffffaf0f`b84b8cf0 fffff800`f9e61190     HyperPlatform!VmmVmExitHandler+0xce
04 ffffaf0f`b84b8d60 fffff800`f9efcf94     HyperPlatform!AsmVmmEntryPoint+0x4d
05 ffff848b`a2909988 fffff800`f9eda459     PCHunter64as+0x6cf94
06 ffff848b`a2909990 fffff800`f9edb775     PCHunter64as+0x4a459
07 ffff848b`a29099e0 fffff800`f9f01611     PCHunter64as+0x4b775
08 ffff848b`a2909a30 fffff807`19e869e9     PCHunter64as+0x71611
09 ffff848b`a2909b30 fffff807`1a402e51     nt!IofCallDriver+0x59
0a ffff848b`a2909b70 fffff807`1a42de5a     nt!IopSynchronousServiceTail+0x1b1
0b ffff848b`a2909c20 fffff807`1a3a4b66     nt!IopXxxControlFile+0x68a
0c ffff848b`a2909d60 fffff807`19fd2885     nt!NtDeviceIoControlFile+0x56
0d ffff848b`a2909dd0 00007ffe`5711f774     nt!KiSystemServiceCopyEnd+0x25
0e 00000000`0013c698 00007ffe`5327ef57     0x00007ffe`5711f774
0f 00000000`0013c6a0 00000000`00000a82     0x00007ffe`5327ef57
 */
  stack->trap_frame.sp = guest_context.gp_regs->sp;
#if 0
  stack->trap_frame.ip = //所有由指令造成的vm-exit都是fault类型,也就是说kGuestRip一定指向造成vm-exit的地址
      guest_context.ip + UtilVmRead(VmcsField::kVmExitInstructionLen);
#endif 
  //
  //其实可以直接这样设置，而不是像上面那样注释的
  //
  stack->trap_frame.ip = guest_context.ip;
  // Dispatch the current VM-exit event
  VmmpHandleVmExit(&guest_context);

  // See: Guidelines for Use of the INVVPID Instruction, and Guidelines for Use
  // of the INVEPT Instruction
  if (!guest_context.vm_continue) {
    UtilInveptGlobal();
    UtilInvvpidAllContext();
  }

  // Restore guest's context
  if (guest_context.irql < DISPATCH_LEVEL) {
    KeLowerIrql(guest_context.irql);
  }

  // Apply possibly updated CR8 by the handler
  if (IsX64()) {
    __writecr8(guest_context.cr8);
  }
  return guest_context.vm_continue;
}
#pragma warning(pop)

// Dispatches VM-exit to a corresponding handler
_Use_decl_annotations_ static void VmmpHandleVmExit(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();

  const VmExitInformation exit_reason = {
      static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason))};

  if (kVmmpEnableRecordVmExit) {
    // Save them for ease of trouble shooting
    const auto processor = KeGetCurrentProcessorNumberEx(nullptr);
    auto &index = g_vmmp_next_history_index[processor];
    auto &history = g_vmmp_vm_exit_history[processor][index];

    history.gp_regs = *guest_context->gp_regs;
    history.ip = guest_context->ip;
    history.exit_reason = exit_reason;
    history.exit_qualification = UtilVmRead(VmcsField::kExitQualification);
    history.instruction_info = UtilVmRead(VmcsField::kVmxInstructionInfo);
    if (++index == kVmmpNumberOfRecords) {
      index = 0;
    }
  }

  switch (exit_reason.fields.reason) {
    case VmxExitReason::kExceptionOrNmi:
      VmmpHandleException(guest_context);
      break;
    case VmxExitReason::kTripleFault:
      VmmpHandleTripleFault(guest_context);
      /* UNREACHABLE */
    case VmxExitReason::kCpuid:
      VmmpHandleCpuid(guest_context);
      break;
    case VmxExitReason::kInvd:
      VmmpHandleInvalidateInternalCaches(guest_context);
      break;
    case VmxExitReason::kInvlpg:
      VmmpHandleInvalidateTlbEntry(guest_context);
      break;
    case VmxExitReason::kRdtsc:
      VmmpHandleRdtsc(guest_context);
      break;
    case VmxExitReason::kCrAccess:
      VmmpHandleCrAccess(guest_context);
      break;
    case VmxExitReason::kDrAccess:
      VmmpHandleDrAccess(guest_context);
      break;
    case VmxExitReason::kIoInstruction:
      VmmpHandleIoPort(guest_context);
      break;
    case VmxExitReason::kMsrRead:
      VmmpHandleMsrReadAccess(guest_context);
      break;
    case VmxExitReason::kMsrWrite:
      VmmpHandleMsrWriteAccess(guest_context);
      break;
    case VmxExitReason::kMonitorTrapFlag:
      VmmpHandleMonitorTrap(guest_context);
      /* UNREACHABLE */
    case VmxExitReason::kGdtrOrIdtrAccess:
      VmmpHandleGdtrOrIdtrAccess(guest_context);
      break;
    case VmxExitReason::kLdtrOrTrAccess:
      VmmpHandleLdtrOrTrAccess(guest_context);
      break;
    case VmxExitReason::kEptViolation:
      VmmpHandleEptViolation(guest_context);
      break;
    case VmxExitReason::kEptMisconfig:
      VmmpHandleEptMisconfig(guest_context);
      /* UNREACHABLE */
    case VmxExitReason::kVmcall:
      VmmpHandleVmCall(guest_context);
      break;
    case VmxExitReason::kVmclear:
    case VmxExitReason::kVmlaunch:
    case VmxExitReason::kVmptrld:
    case VmxExitReason::kVmptrst:
    case VmxExitReason::kVmread:
    case VmxExitReason::kVmresume:
    case VmxExitReason::kVmwrite:
    case VmxExitReason::kVmoff:
    case VmxExitReason::kVmon:
    case VmxExitReason::kInvept:
    case VmxExitReason::kInvvpid:
      VmmpHandleVmx(guest_context);
      break;
    case VmxExitReason::kRdtscp:
      VmmpHandleRdtscp(guest_context);
      break;
    case VmxExitReason::kXsetbv:
      VmmpHandleXsetbv(guest_context);
      break;
    default:
      VmmpHandleUnexpectedExit(guest_context); 
      /* UNREACHABLE */
  }
}

// Triple fault VM-exit. Fatal error.
_Use_decl_annotations_ static void VmmpHandleTripleFault(
    GuestContext *guest_context) {
  VmmpDumpGuestState();
  HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kTripleFaultVmExit,
                                 reinterpret_cast<ULONG_PTR>(guest_context),
                                 guest_context->ip, 0);
}

// Unexpected VM-exit. Fatal error.
_Use_decl_annotations_ static void VmmpHandleUnexpectedExit(
    GuestContext *guest_context) {
  VmmpDumpGuestState();
  //
  //未被接管的vm-exit
  //
  const auto qualification = UtilVmRead(VmcsField::kExitQualification);
  HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnexpectedVmExit,
                                 reinterpret_cast<ULONG_PTR>(guest_context),
                                 guest_context->ip, qualification);
}

// MTF VM-exit
_Use_decl_annotations_ static void VmmpHandleMonitorTrap(
    GuestContext *guest_context) {
  VmmpDumpGuestState();
  HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnexpectedVmExit,
                                 reinterpret_cast<ULONG_PTR>(guest_context),
                                 guest_context->ip, 0);
}

// Interrupt
_Use_decl_annotations_ static void VmmpHandleException(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  const VmExitInterruptionInformationField exception = {
      static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))};
  const auto interruption_type =
      static_cast<InterruptionType>(exception.fields.interruption_type);
  const auto vector = static_cast<InterruptionVector>(exception.fields.vector);

  if (interruption_type == InterruptionType::kHardwareException) {
    // Hardware exception
    if (vector == InterruptionVector::kPageFaultException) {
      // #PF
      const PageFaultErrorCode fault_code = {
          static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode))};
      const auto fault_address = UtilVmRead(VmcsField::kExitQualification);

      VmmpInjectInterruption(interruption_type, vector, true, fault_code.all);
      HYPERPLATFORM_LOG_INFO_SAFE(
          "GuestIp= %016Ix, #PF Fault= %016Ix Code= 0x%2x", guest_context->ip,
          fault_address, fault_code.all);
      AsmWriteCR2(fault_address);

    } else if (vector == InterruptionVector::kGeneralProtectionException) {
      // # GP
      const auto error_code =
          static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode));

      VmmpInjectInterruption(interruption_type, vector, true, error_code);
      HYPERPLATFORM_LOG_INFO_SAFE("GuestIp= %016Ix, #GP Code= 0x%2x",
                                  guest_context->ip, error_code);

    } else {
      HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
                                     0);
    }

  } else if (interruption_type == InterruptionType::kSoftwareException) {
    // Software exception
    if (vector == InterruptionVector::kBreakpointException) {
      // #BP
      VmmpInjectInterruption(interruption_type, vector, false, 0);
      HYPERPLATFORM_LOG_INFO_SAFE("GuestIp= %016Ix, #BP ", guest_context->ip);
      const auto exit_inst_length =
          UtilVmRead(VmcsField::kVmExitInstructionLen);
      UtilVmWrite(VmcsField::kVmEntryInstructionLen, exit_inst_length);

      /*
      * Q:思考一下如果不给客户机注入int3会发生什么事情？
      * A:guest -> int3 -> vmm -> #bp handler 如果handler不调整guest rip的话，guest就会一直执行int3
      */
#if 0 //如果调用VmmpInjectInterruption注入异常，那么guest的rip就由guest来调整，也就是vmm不干涉，这样是比较理想的。
      VmmpAdjustGuestInstructionPointer(guest_context);
#endif 


    } else {
      HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
                                     0);
    }
  } else {
    HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
                                   0);
  }
}

// CPUID
_Use_decl_annotations_ static void VmmpHandleCpuid(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();

  unsigned int cpu_info[4] = {};
  const auto function_id = static_cast<int>(guest_context->gp_regs->ax);
  const auto sub_function_id = static_cast<int>(guest_context->gp_regs->cx);

  //VMM替VM执行一遍cpuid，并返回相应结果
  __cpuidex(reinterpret_cast<int *>(cpu_info), function_id, sub_function_id);

  //
  //用虚拟机管理员位来提示VMM存在
  //用硬件保留的id号来返回vmm标识
  //

  //
  //这里先选择不做处理，如果有需要伪造返回数据就可以再改
  //
#if 0
  if (function_id == 1) {
    // Present existence of a hypervisor using the HypervisorPresent bit
    CpuFeaturesEcx cpu_features = {static_cast<ULONG32>(cpu_info[2])};
    cpu_features.fields.not_used = true;
    cpu_info[2] = static_cast<int>(cpu_features.all);
  } else if (function_id == kHyperVCpuidInterface) {
    // Leave signature of HyperPlatform onto EAX
    cpu_info[0] = 'PpyH';
  }
#endif
  //
  //https://www.deepinstinct.com/blog/malware-evasion-techniques-part-2-anti-vm-blog
  //解决cpuid的示例1示例2
  //

  if (function_id == 0) {
      guest_context->gp_regs->ax = 16;
      guest_context->gp_regs->bx = 0x756E6547;
      guest_context->gp_regs->cx = 0x6C65746E;
      guest_context->gp_regs->dx = 0x49656E69;

      VmmpAdjustGuestInstructionPointer(guest_context);
      return;
  }
  
  //cpuid.1.ecx="0-:--:--:--:--:--:--:--"
  if (function_id == 1) {
      // Present existence of a hypervisor using the HypervisorPresent bit
      CpuFeaturesEcx cpu_features = { static_cast<ULONG32>(cpu_info[2]) };
      cpu_features.fields.not_used = false;//指示虚拟机不存在
      cpu_info[2] = static_cast<int>(cpu_features.all);
  }

  //
  //VMX文件中加入，替换默认的cpuid 40000000返回值
  //cpuid.40000000.ecx=”0000:0000:0000:0000:0000:0000:0000:0000”
  //cpuid.40000000.edx = ”0000:0000 : 0000 : 0000 : 0000 : 0000 : 0000 : 0000”
  //

  guest_context->gp_regs->ax = cpu_info[0];
  guest_context->gp_regs->bx = cpu_info[1];
  guest_context->gp_regs->cx = cpu_info[2];
  guest_context->gp_regs->dx = cpu_info[3];

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// RDTSC
// 一般来说hypervisor都不会让rdtsc vm-exit
_Use_decl_annotations_ static void VmmpHandleRdtsc(
    GuestContext *guest_context) {
    HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
    ULARGE_INTEGER tsc = {};
    tsc.QuadPart = __rdtsc();
    guest_context->gp_regs->dx = tsc.HighPart;
    guest_context->gp_regs->ax = tsc.LowPart;

    VmmpAdjustGuestInstructionPointer(guest_context);
}

// RDTSCP
_Use_decl_annotations_ static void VmmpHandleRdtscp(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  unsigned int tsc_aux = 0;
  ULARGE_INTEGER tsc = {};
  tsc.QuadPart = __rdtscp(&tsc_aux);
  guest_context->gp_regs->dx = tsc.HighPart;
  guest_context->gp_regs->ax = tsc.LowPart;
  guest_context->gp_regs->cx = tsc_aux;

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// XSETBV. It is executed at the time of system resuming
_Use_decl_annotations_ static void VmmpHandleXsetbv(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  ULARGE_INTEGER value = {};
  value.LowPart = static_cast<ULONG>(guest_context->gp_regs->ax);
  value.HighPart = static_cast<ULONG>(guest_context->gp_regs->dx);
  _xsetbv(static_cast<ULONG>(guest_context->gp_regs->cx), value.QuadPart);

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// RDMSR
_Use_decl_annotations_ static void VmmpHandleMsrReadAccess(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  VmmpHandleMsrAccess(guest_context, true);
}

// WRMSR
_Use_decl_annotations_ static void VmmpHandleMsrWriteAccess(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  VmmpHandleMsrAccess(guest_context, false);
}

// RDMSR and WRMSR
/*
Code that accesses a model-specific MSR and that is executed on a processor that does not support that MSR will
generate an exception.
 
MSR address range between 40000000H - 400000FFH is marked as a specially reserved range. All existing and
future processors will not implement any features using any MSR in this range.
*/
_Use_decl_annotations_ static void VmmpHandleMsrAccess(
    GuestContext *guest_context, bool read_access) {
  // Apply it for VMCS instead of a real MSR if a specified MSR is either of
  // them.
  const auto msr = static_cast<Msr>(guest_context->gp_regs->cx);

  bool is_vaild_msr = false;
  //
  //对不支持的msr注入#gp
  //

  if (guest_context->gp_regs->cx <= 0x1FFF)
      is_vaild_msr = true;
  else if ((guest_context->gp_regs->cx >= 0xC0000000) && (guest_context->gp_regs->cx <= 0xC0001FFF))
      is_vaild_msr = true;


  if (!is_vaild_msr
#ifdef VMWARE //在vmware上测试要加上，有个叫PpmIdleGuestExecute会rdmsr 0x400000F0u
      && (guest_context->gp_regs->cx != 0x400000f0)
#endif
      )
  {
      /*
      * 在vmware下，不支持的msr，会返回未定义的值，而不会蓝屏
      * 真实机子下这样的话不try-catch是要蓝屏的
      */
#if 1
      VmmpInjectInterruption(
          InterruptionType::kHardwareException,
          InterruptionVector::kGeneralProtectionException,
          true,
          0x6A);

      VmmpAdjustGuestInstructionPointer(guest_context);
      return;
#endif 
  }

  //
  //对正常的msr提供服务
  //

  bool transfer_to_vmcs = false;
  VmcsField vmcs_field = {};
  switch (msr) {
    case Msr::kIa32SysenterCs:
      vmcs_field = VmcsField::kGuestSysenterCs;
      transfer_to_vmcs = true;
      break;
    case Msr::kIa32SysenterEsp:
      vmcs_field = VmcsField::kGuestSysenterEsp;
      transfer_to_vmcs = true;
      break;
    case Msr::kIa32SysenterEip:
      vmcs_field = VmcsField::kGuestSysenterEip;
      transfer_to_vmcs = true;
      break;
    case Msr::kIa32Debugctl:
      vmcs_field = VmcsField::kGuestIa32Debugctl;
      transfer_to_vmcs = true;
      break;
    case Msr::kIa32GsBase:
      vmcs_field = VmcsField::kGuestGsBase;
      transfer_to_vmcs = true;
      break;
    case Msr::kIa32FsBase:
      vmcs_field = VmcsField::kGuestFsBase;
      transfer_to_vmcs = true;
      break;
    default:
      break;
  }

  const auto is_64bit_vmcs =
      UtilIsInBounds(vmcs_field, VmcsField::kIoBitmapA,
                     VmcsField::kHostIa32PerfGlobalCtrlHigh);

  LARGE_INTEGER msr_value = {};
  if (read_access) {
    if (transfer_to_vmcs) {
      if (is_64bit_vmcs) {
        msr_value.QuadPart = UtilVmRead64(vmcs_field);
      } else {
        msr_value.QuadPart = UtilVmRead(vmcs_field);
      }
    } else {
      msr_value.QuadPart = UtilReadMsr64(msr);
    }
    guest_context->gp_regs->ax = msr_value.LowPart;
    guest_context->gp_regs->dx = msr_value.HighPart;
  } else {//写msr
    msr_value.LowPart = static_cast<ULONG>(guest_context->gp_regs->ax);
    msr_value.HighPart = static_cast<ULONG>(guest_context->gp_regs->dx);
    if (transfer_to_vmcs) {
      if (is_64bit_vmcs) {
        UtilVmWrite64(vmcs_field, static_cast<ULONG_PTR>(msr_value.QuadPart));
      } else {
        UtilVmWrite(vmcs_field, static_cast<ULONG_PTR>(msr_value.QuadPart));
      }
    } else {
      UtilWriteMsr64(msr, msr_value.QuadPart);
    }
  }

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// LIDT, SIDT, LGDT and SGDT
_Use_decl_annotations_ static void VmmpHandleGdtrOrIdtrAccess(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  const GdtrOrIdtrInstInformation instruction_info = {
      static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo))};

  // Calculate an address to be used for the instruction
  const auto displacement = UtilVmRead(VmcsField::kExitQualification);

  // Base
  ULONG_PTR base_value = 0;
  if (!instruction_info.fields.base_register_invalid) {
    const auto register_used = VmmpSelectRegister(
        instruction_info.fields.base_register, guest_context);
    base_value = *register_used;
  }

  // Index
  ULONG_PTR index_value = 0;
  if (!instruction_info.fields.index_register_invalid) {
    const auto register_used = VmmpSelectRegister(
        instruction_info.fields.index_register, guest_context);
    index_value = *register_used;
    switch (static_cast<Scaling>(instruction_info.fields.scalling)) {
      case Scaling::kScaleBy2:
        index_value = index_value * 2;
        break;
      case Scaling::kScaleBy4:
        index_value = index_value * 4;
        break;
      case Scaling::kScaleBy8:
        index_value = index_value * 8;
        break;
      default:
        break;
    }
  }

  // clang-format off
  ULONG_PTR segment_base = 0;
  switch (instruction_info.fields.segment_register) {
    case 0: segment_base = UtilVmRead(VmcsField::kGuestEsBase); break;
    case 1: segment_base = UtilVmRead(VmcsField::kGuestCsBase); break;
    case 2: segment_base = UtilVmRead(VmcsField::kGuestSsBase); break;
    case 3: segment_base = UtilVmRead(VmcsField::kGuestDsBase); break;
    case 4: segment_base = UtilVmRead(VmcsField::kGuestFsBase); break;
    case 5: segment_base = UtilVmRead(VmcsField::kGuestGsBase); break;
    default: HYPERPLATFORM_COMMON_DBG_BREAK(); break;
  }
  // clang-format on

  auto operation_address =
      segment_base + base_value + index_value + displacement;
  if (static_cast<AddressSize>(instruction_info.fields.address_size) ==
      AddressSize::k32bit) {
    operation_address &= MAXULONG;
  }

  // Update CR3 with that of the guest since below code is going to access
  // memory.
  const auto guest_cr3 = VmmpGetKernelCr3();
  const auto vmm_cr3 = __readcr3();
  __writecr3(guest_cr3);

  // Emulate the instruction
  auto descriptor_table_reg = reinterpret_cast<Idtr *>(operation_address);
  switch (static_cast<GdtrOrIdtrInstructionIdentity>(
      instruction_info.fields.instruction_identity)) {
    case GdtrOrIdtrInstructionIdentity::kSgdt: {
      // On 64bit system, SIDT and SGDT can be executed from a 32bit process
      // where runs with the 32bit operand size. The following checks the
      // current guest's operand size and writes either full 10 bytes (for the
      // 64bit more) or 6 bytes or IDTR or GDTR as the processor does. See:
      // Operand Size and Address Size in 64-Bit Mode See: SGDT-Store Global
      // Descriptor Table Register See: SIDT-Store Interrupt Descriptor Table
      // Register
      const auto gdt_base = UtilVmRead(VmcsField::kGuestGdtrBase);
      const auto gdt_limit =
          static_cast<unsigned short>(UtilVmRead(VmcsField::kGuestGdtrLimit));

      const SegmentSelector ss = {
          static_cast<USHORT>(UtilVmRead(VmcsField::kGuestCsSelector))};
      const auto segment_descriptor = reinterpret_cast<SegmentDescriptor *>(
          gdt_base + ss.fields.index * sizeof(SegmentDescriptor));
      if (segment_descriptor->fields.l) {
        // 64bit
        descriptor_table_reg->base = gdt_base;
        descriptor_table_reg->limit = gdt_limit;
      } else {
        // 32bit
        const auto descriptor_table_reg32 =
            reinterpret_cast<Idtr32 *>(descriptor_table_reg);
        descriptor_table_reg32->base = static_cast<ULONG32>(gdt_base);
        descriptor_table_reg32->limit = gdt_limit;
      }
      break;
    }
    case GdtrOrIdtrInstructionIdentity::kSidt: {
      const auto idt_base = UtilVmRead(VmcsField::kGuestIdtrBase);
      const auto idt_limit =
          static_cast<unsigned short>(UtilVmRead(VmcsField::kGuestIdtrLimit));

      const auto gdt_base = UtilVmRead(VmcsField::kGuestGdtrBase);
      const SegmentSelector ss = {
          static_cast<USHORT>(UtilVmRead(VmcsField::kGuestCsSelector))};
      const auto segment_descriptor = reinterpret_cast<SegmentDescriptor *>(
          gdt_base + ss.fields.index * sizeof(SegmentDescriptor));
      if (segment_descriptor->fields.l) {
        // 64bit
        descriptor_table_reg->base = idt_base;
        descriptor_table_reg->limit = idt_limit;
      } else {
        // 32bit
        const auto descriptor_table_reg32 =
            reinterpret_cast<Idtr32 *>(descriptor_table_reg);
        descriptor_table_reg32->base = static_cast<ULONG32>(idt_base);
        descriptor_table_reg32->limit = idt_limit;
      }
      break;
    }
    case GdtrOrIdtrInstructionIdentity::kLgdt:
      UtilVmWrite(VmcsField::kGuestGdtrBase, descriptor_table_reg->base);
      UtilVmWrite(VmcsField::kGuestGdtrLimit, descriptor_table_reg->limit);
      break;
    case GdtrOrIdtrInstructionIdentity::kLidt:
      UtilVmWrite(VmcsField::kGuestIdtrBase, descriptor_table_reg->base);
      UtilVmWrite(VmcsField::kGuestIdtrLimit, descriptor_table_reg->limit);
      break;
  }

  __writecr3(vmm_cr3);
  VmmpAdjustGuestInstructionPointer(guest_context);
}

// LLDT, LTR, SLDT, and STR
_Use_decl_annotations_ static void VmmpHandleLdtrOrTrAccess(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  const LdtrOrTrInstInformation instruction_info = {
      static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo))};

  // Calculate an address or a register to be used for the instruction
  const auto displacement = UtilVmRead(VmcsField::kExitQualification);

  ULONG_PTR operation_address = 0;
  if (instruction_info.fields.register_access) {
    // Register
    const auto register_used =
        VmmpSelectRegister(instruction_info.fields.register1, guest_context);
    operation_address = reinterpret_cast<ULONG_PTR>(register_used);
  } else {
    // Base
    ULONG_PTR base_value = 0;
    if (!instruction_info.fields.base_register_invalid) {
      const auto register_used = VmmpSelectRegister(
          instruction_info.fields.base_register, guest_context);
      base_value = *register_used;
    }

    // Index
    ULONG_PTR index_value = 0;
    if (!instruction_info.fields.index_register_invalid) {
      const auto register_used = VmmpSelectRegister(
          instruction_info.fields.index_register, guest_context);
      index_value = *register_used;
      switch (static_cast<Scaling>(instruction_info.fields.scalling)) {
        case Scaling::kScaleBy2:
          index_value = index_value * 2;
          break;
        case Scaling::kScaleBy4:
          index_value = index_value * 4;
          break;
        case Scaling::kScaleBy8:
          index_value = index_value * 8;
          break;
        default:
          break;
      }
    }

    // clang-format off
    ULONG_PTR segment_base = 0;
    switch (instruction_info.fields.segment_register) {
      case 0: segment_base = UtilVmRead(VmcsField::kGuestEsBase); break;
      case 1: segment_base = UtilVmRead(VmcsField::kGuestCsBase); break;
      case 2: segment_base = UtilVmRead(VmcsField::kGuestSsBase); break;
      case 3: segment_base = UtilVmRead(VmcsField::kGuestDsBase); break;
      case 4: segment_base = UtilVmRead(VmcsField::kGuestFsBase); break;
      case 5: segment_base = UtilVmRead(VmcsField::kGuestGsBase); break;
      default: HYPERPLATFORM_COMMON_DBG_BREAK(); break;
    }
    // clang-format on

    operation_address = segment_base + base_value + index_value + displacement;
    if (static_cast<AddressSize>(instruction_info.fields.address_size) ==
        AddressSize::k32bit) {
      operation_address &= MAXULONG;
    }
  }

  // Update CR3 with that of the guest since below code is going to access
  // memory.
  const auto guest_cr3 = VmmpGetKernelCr3();
  const auto vmm_cr3 = __readcr3();
  __writecr3(guest_cr3);

  // Emulate the instruction
  auto selector = reinterpret_cast<USHORT *>(operation_address);
  switch (static_cast<LdtrOrTrInstructionIdentity>(
      instruction_info.fields.instruction_identity)) {
    case LdtrOrTrInstructionIdentity::kSldt:
      *selector =
          static_cast<USHORT>(UtilVmRead(VmcsField::kGuestLdtrSelector));
      break;
    case LdtrOrTrInstructionIdentity::kStr:
      *selector = static_cast<USHORT>(UtilVmRead(VmcsField::kGuestTrSelector));
      break;
    case LdtrOrTrInstructionIdentity::kLldt:
      UtilVmWrite(VmcsField::kGuestLdtrSelector, *selector);
      break;
    case LdtrOrTrInstructionIdentity::kLtr: {
      UtilVmWrite(VmcsField::kGuestTrSelector, *selector);
      // Set the Busy bit in TSS.
      // See: LTR - Load Task Register
      const SegmentSelector ss = {*selector};
      const auto sd = reinterpret_cast<SegmentDescriptor *>(
          UtilVmRead(VmcsField::kGuestGdtrBase) +
          ss.fields.index * sizeof(SegmentDescriptor));
      sd->fields.type |= 2;  // Set the Busy bit
      break;
    }
  }

  __writecr3(vmm_cr3);
  VmmpAdjustGuestInstructionPointer(guest_context);
}

// MOV to / from DRx
_Use_decl_annotations_ static void VmmpHandleDrAccess(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();

  // Normally, when the privileged instruction is executed at CPL3, #GP(0)
  // occurs instead of VM-exit. However, access to the debug registers is
  // exception. Inject #GP(0) in such case to emulate what the processor
  // normally does. See: Instructions That Cause VM Exits Conditionally
  if (VmmpGetGuestCpl() != 0) {
    VmmpInjectInterruption(InterruptionType::kHardwareException,
                           InterruptionVector::kGeneralProtectionException,
                           true, 0);
    return;
  }

  const MovDrQualification exit_qualification = {
      UtilVmRead(VmcsField::kExitQualification)};
  auto debugl_register = exit_qualification.fields.debugl_register;

  // Access to DR4 and 5 causes #UD when CR4.DE (Debugging Extensions) is set.
  // Otherwise, these registers are aliased to DR6 and 7 respectively.
  // See: Debug Registers DR4 and DR5
  if (debugl_register == 4 || debugl_register == 5) {
    const Cr4 guest_cr4 = {UtilVmRead(VmcsField::kGuestCr4)};
    if (guest_cr4.fields.de) {
      VmmpInjectInterruption(InterruptionType::kHardwareException,
                             InterruptionVector::kInvalidOpcodeException, false,
                             0);
      return;
    } else if (debugl_register == 4) {
      debugl_register = 6;
    } else {
      debugl_register = 7;
    }
  }

  // Access to any of DRs causes #DB when DR7.GD (General Detect Enable) is set.
  // See: Debug Control Register (DR7)
  Dr7 guest_dr7 = {UtilVmRead(VmcsField::kGuestDr7)};
  if (guest_dr7.fields.gd) {
    Dr6 guest_dr6 = {__readdr(6)};
    // Clear DR6.B0-3 since the #DB being injected is not due to match of a
    // condition specified in DR6. The processor is allowed to clear those bits
    // as "Certain debug exceptions may clear bits 0-3."
    guest_dr6.fields.b0 = false;
    guest_dr6.fields.b1 = false;
    guest_dr6.fields.b2 = false;
    guest_dr6.fields.b3 = false;
    // "When such a condition is detected, the BD flag in debug status register
    // DR6 is set prior to generating the exception."
    guest_dr6.fields.bd = true;
    __writedr(6, guest_dr6.all);

    VmmpInjectInterruption(InterruptionType::kHardwareException,
                           InterruptionVector::kDebugException, false, 0);

    // While the processor clears the DR7.GD bit on #DB ("The processor clears
    // the GD flag upon entering to the debug exception handler"), it does not
    // change that in the VMCS. Emulate that behavior here. Note that this bit
    // should actually be cleared by intercepting #DB and in the handler instead
    // of here, since the processor clears it on any #DB. We do not do that as
    // we do not intercept #DB as-is.
    guest_dr7.fields.gd = false;
    UtilVmWrite(VmcsField::kGuestDr7, guest_dr7.all);
    return;
  }

  const auto register_used =
      VmmpSelectRegister(exit_qualification.fields.gp_register, guest_context);
  const auto direction =
      static_cast<MovDrDirection>(exit_qualification.fields.direction);

  // In 64-bit mode, the upper 32 bits of DR6 and DR7 are reserved and must be
  // written with zeros. Writing 1 to any of the upper 32 bits results in a
  // #GP(0) exception. See: Debug Registers and Intel® 64 Processors
  if (IsX64() && direction == MovDrDirection::kMoveToDr) {
    const auto value64 = static_cast<ULONG64>(*register_used);
    if ((debugl_register == 6 || debugl_register == 7) && (value64 >> 32)) {
      VmmpInjectInterruption(InterruptionType::kHardwareException,
                             InterruptionVector::kGeneralProtectionException,
                             true, 0);
      return;
    }
  }

  switch (direction) {
    case MovDrDirection::kMoveToDr:
      switch (debugl_register) {
        // clang-format off
        case 0: __writedr(0, *register_used); break;
        case 1: __writedr(1, *register_used); break;
        case 2: __writedr(2, *register_used); break;
        case 3: __writedr(3, *register_used); break;
        // clang-format on
        case 6: {
          // Make sure that we write 0 and 1 into the bits that are stated to be
          // so. The Intel SDM does not appear to state what happens when the
          // processor attempts to write 1 to the always 0 bits, and vice versa,
          // however, observation is that writes to those bits are ignored
          // *as long as it is done on the non-root mode*, and other hypervisors
          // emulate in that way as well.
          Dr6 write_value = {*register_used};
          write_value.fields.reserved1 |= ~write_value.fields.reserved1;
          write_value.fields.reserved2 = 0;
          write_value.fields.reserved3 |= ~write_value.fields.reserved3;
          __writedr(6, write_value.all);
          break;
        }
        case 7: {
          // Similar to the case of CR6, enforce always 1 and 0 behavior.
          Dr7 write_value = {*register_used};
          write_value.fields.reserved1 |= ~write_value.fields.reserved1;
          write_value.fields.reserved2 = 0;
          write_value.fields.reserved3 = 0;
          UtilVmWrite(VmcsField::kGuestDr7, write_value.all);
          break;
        }
        default:
          break;
      }
      break;
    case MovDrDirection::kMoveFromDr:
      // clang-format off
      switch (debugl_register) {
        case 0: *register_used = __readdr(0); break;
        case 1: *register_used = __readdr(1); break;
        case 2: *register_used = __readdr(2); break;
        case 3: *register_used = __readdr(3); break;
        case 6: *register_used = __readdr(6); break;
        case 7: *register_used = UtilVmRead(VmcsField::kGuestDr7); break;
        default: break;
      }
      // clang-format on
      break;
  }

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// IN, INS, OUT, OUTS
_Use_decl_annotations_ static void VmmpHandleIoPort(
    GuestContext *guest_context) {
  const IoInstQualification exit_qualification = {
      UtilVmRead(VmcsField::kExitQualification)};

  const auto is_in = exit_qualification.fields.direction == 1;  // to memory?
  const auto is_string = exit_qualification.fields.string_instruction == 1;
  const auto is_rep = exit_qualification.fields.rep_prefixed == 1;
  const auto port = static_cast<USHORT>(exit_qualification.fields.port_number);
  const auto string_address = reinterpret_cast<void *>(
      (is_in) ? guest_context->gp_regs->di : guest_context->gp_regs->si);
  const auto count =
      static_cast<unsigned long>((is_rep) ? guest_context->gp_regs->cx : 1);
  const auto address =
      (is_string) ? string_address : &guest_context->gp_regs->ax;

  SIZE_T size_of_access = 0;
  const char *suffix = "";
  switch (static_cast<IoInstSizeOfAccess>(
      exit_qualification.fields.size_of_access)) {
    case IoInstSizeOfAccess::k1Byte:
      size_of_access = 1;
      suffix = "B";
      break;
    case IoInstSizeOfAccess::k2Byte:
      size_of_access = 2;
      suffix = "W";
      break;
    case IoInstSizeOfAccess::k4Byte:
      size_of_access = 4;
      suffix = "D";
      break;
  }

  HYPERPLATFORM_LOG_DEBUG_SAFE("GuestIp= %016Ix, Port= %04x, %s%s%s",
                               guest_context->ip, port, (is_in ? "IN" : "OUT"),
                               (is_string ? "S" : ""),
                               (is_string ? suffix : ""));

  VmmpIoWrapper(is_in, is_string, size_of_access, port, address, count);

  // Update RCX, RDI and RSI accordingly. Note that this code can handle only
  // the REP prefix.
  if (is_string) {
    const auto update_count = (is_rep) ? guest_context->gp_regs->cx : 1;
    const auto update_size = update_count * size_of_access;
    const auto update_register =
        (is_in) ? &guest_context->gp_regs->di : &guest_context->gp_regs->si;

    if (guest_context->flag_reg.fields.df) {
      *update_register = *update_register - update_size;
    } else {
      *update_register = *update_register + update_size;
    }

    if (is_rep) {
      guest_context->gp_regs->cx = 0;
    }
  }

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// Perform IO instruction according with parameters
_Use_decl_annotations_ static void VmmpIoWrapper(bool to_memory, bool is_string,
                                                 SIZE_T size_of_access,
                                                 unsigned short port,
                                                 void *address,
                                                 unsigned long count) {
  NT_ASSERT(size_of_access == 1 || size_of_access == 2 || size_of_access == 4);

  // Update CR3 with that of the guest since below code is going to access
  // memory.
  const auto guest_cr3 = VmmpGetKernelCr3();
  const auto vmm_cr3 = __readcr3();
  __writecr3(guest_cr3);

  // clang-format off
  if (to_memory) {
    if (is_string) {
      // INS
      switch (size_of_access) {
      case 1: __inbytestring(port, static_cast<UCHAR*>(address), count); break;
      case 2: __inwordstring(port, static_cast<USHORT*>(address), count); break;
      case 4: __indwordstring(port, static_cast<ULONG*>(address), count); break;
      default: HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0, 0);
      /* UNREACHABLE */
      }
    } else {
      // IN
      switch (size_of_access) {
      case 1: *static_cast<UCHAR*>(address) = __inbyte(port); break;
      case 2: *static_cast<USHORT*>(address) = __inword(port); break;
      case 4: *static_cast<ULONG*>(address) = __indword(port); break;
      default: HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0, 0);
      /* UNREACHABLE */
      }
    }
  } else {
    if (is_string) {
      // OUTS
      switch (size_of_access) {
      case 1: __outbytestring(port, static_cast<UCHAR*>(address), count); break;
      case 2: __outwordstring(port, static_cast<USHORT*>(address), count); break;
      case 4: __outdwordstring(port, static_cast<ULONG*>(address), count); break;
      default: HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0, 0);
      /* UNREACHABLE */
      }
    } else {
      // OUT
      switch (size_of_access) {
      case 1: __outbyte(port, *static_cast<UCHAR*>(address)); break;
      case 2: __outword(port, *static_cast<USHORT*>(address)); break;
      case 4: __outdword(port, *static_cast<ULONG*>(address)); break;
      default: HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0, 0);
      /* UNREACHABLE */
      }
    }
  }
  // clang-format on

  __writecr3(vmm_cr3);
}

// MOV to / from CRx
_Use_decl_annotations_ static void VmmpHandleCrAccess(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();

  const MovCrQualification exit_qualification = {
      UtilVmRead(VmcsField::kExitQualification)};

  const auto register_used =
      VmmpSelectRegister(exit_qualification.fields.gp_register, guest_context);

  switch (static_cast<MovCrAccessType>(exit_qualification.fields.access_type)) {
    case MovCrAccessType::kMoveToCr:
      switch (exit_qualification.fields.control_register) {
        // CR0 <- Reg
        case 0: {
          HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
          if (UtilIsX86Pae()) {
            UtilLoadPdptes(UtilVmRead(VmcsField::kGuestCr3));
          }
          const Cr0 cr0_fixed0 = {UtilReadMsr(Msr::kIa32VmxCr0Fixed0)};
          const Cr0 cr0_fixed1 = {UtilReadMsr(Msr::kIa32VmxCr0Fixed1)};
          Cr0 cr0 = {*register_used};
          cr0.all &= cr0_fixed1.all;
          cr0.all |= cr0_fixed0.all;
          UtilVmWrite(VmcsField::kGuestCr0, cr0.all);
          UtilVmWrite(VmcsField::kCr0ReadShadow, cr0.all);
          break;
        }

        // CR3 <- Reg
        case 3: {
          HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
          if (UtilIsX86Pae()) {
            UtilLoadPdptes(VmmpGetKernelCr3());
          }
          // Under some circumstances MOV to CR3 is not *required* to flush TLB
          // entries, but also NOT prohibited to do so. Therefore, we flush it
          // all time.
          // See: Operations that Invalidate TLBs and Paging-Structure Caches
          UtilInvvpidSingleContextExceptGlobal(
              static_cast<USHORT>(KeGetCurrentProcessorNumberEx(nullptr) + 1));

          // The MOV to CR3 does not modify the bit63 of CR3. Emulate this
          // behavior.
          // See: MOV - Move to/from Control Registers
          UtilVmWrite(VmcsField::kGuestCr3, (*register_used & ~(1ULL << 63)));
          break;
        }

        // CR4 <- Reg
        case 4: {
          HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
          if (UtilIsX86Pae()) {
            UtilLoadPdptes(UtilVmRead(VmcsField::kGuestCr3));
          }
          UtilInvvpidAllContext();
          const Cr4 cr4_fixed0 = {UtilReadMsr(Msr::kIa32VmxCr4Fixed0)};
          const Cr4 cr4_fixed1 = {UtilReadMsr(Msr::kIa32VmxCr4Fixed1)};
          Cr4 cr4 = {*register_used};
          cr4.all &= cr4_fixed1.all;
          cr4.all |= cr4_fixed0.all;
          UtilVmWrite(VmcsField::kGuestCr4, cr4.all);
          UtilVmWrite(VmcsField::kCr4ReadShadow, cr4.all);
          break;
        }

        // CR8 <- Reg
        case 8: {
          HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
          guest_context->cr8 = *register_used;
          break;
        }

        default:
          HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0,
                                         0, 0);
          /* UNREACHABLE */
      }
      break;

    case MovCrAccessType::kMoveFromCr:
      switch (exit_qualification.fields.control_register) {
        // Reg <- CR3
        case 3: {
          HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
          *register_used = UtilVmRead(VmcsField::kGuestCr3);
          break;
        }

        // Reg <- CR8
        case 8: {
          HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
          *register_used = guest_context->cr8;
          break;
        }

        default:
          HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0,
                                         0, 0);
          /* UNREACHABLE */
      }
      break;

    // Unimplemented
    case MovCrAccessType::kClts:
    case MovCrAccessType::kLmsw:
      HYPERPLATFORM_COMMON_DBG_BREAK();
      break;
  }

  VmmpAdjustGuestInstructionPointer(guest_context);
}

// VMX instructions except for VMCALL
_Use_decl_annotations_ static void VmmpHandleVmx(GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  // See: CONVENTIONS
  guest_context->flag_reg.fields.cf = true;  // Error without status
  guest_context->flag_reg.fields.pf = false;
  guest_context->flag_reg.fields.af = false;
  guest_context->flag_reg.fields.zf = false;  // Error without status
  guest_context->flag_reg.fields.sf = false;
  guest_context->flag_reg.fields.of = false;
  UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);
  VmmpAdjustGuestInstructionPointer(guest_context);
}

// VMCALL
_Use_decl_annotations_ static void VmmpHandleVmCall(
    GuestContext *guest_context) {
  // VMCALL convention for HyperPlatform:
  //  ecx: hyper-call number (always 32bit)
  //  edx: arbitrary context parameter (pointer size)
  // Any unsuccessful VMCALL will inject #UD into a guest
  const auto hypercall_number =
      static_cast<HypercallNumber>(guest_context->gp_regs->cx);
  const auto context = reinterpret_cast<void *>(guest_context->gp_regs->dx);

  if (!UtilIsInBounds(hypercall_number,
                      HypercallNumber::kMinimumHypercallNumber,
                      HypercallNumber::kMaximumHypercallNumber)) {
    // Unsupported hypercall
    VmmpIndicateUnsuccessfulVmcall(guest_context);
  }

  switch (hypercall_number) {
    case HypercallNumber::kTerminateVmm:
      // Unloading requested. This VMCALL is allowed to execute only from CPL=0
      if (VmmpGetGuestCpl() == 0) {
        VmmpHandleVmCallTermination(guest_context, context);
      } else {//处理三环的VMCALL，注入一个异常给guest执行
        VmmpIndicateUnsuccessfulVmcall(guest_context);
      }
      break;
    case HypercallNumber::kPingVmm:
      // Sample VMCALL handler
      HYPERPLATFORM_LOG_INFO_SAFE("Pong by VMM! (context = %p)", context);
      VmmpIndicateSuccessfulVmcall(guest_context);
      break;
    case HypercallNumber::kGetSharedProcessorData:
      *static_cast<void **>(context) =
          guest_context->stack->processor_data->shared_data;
      VmmpIndicateSuccessfulVmcall(guest_context);
      break;
  }
}

// INVD
_Use_decl_annotations_ static void VmmpHandleInvalidateInternalCaches(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  AsmInvalidateInternalCaches();
  VmmpAdjustGuestInstructionPointer(guest_context);
}

// INVLPG
_Use_decl_annotations_ static void VmmpHandleInvalidateTlbEntry(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  const auto invalidate_address =
      reinterpret_cast<void *>(UtilVmRead(VmcsField::kExitQualification));
  UtilInvvpidIndividualAddress(
      static_cast<USHORT>(KeGetCurrentProcessorNumberEx(nullptr) + 1),
      invalidate_address);
  VmmpAdjustGuestInstructionPointer(guest_context);
}

// EXIT_REASON_EPT_VIOLATION
_Use_decl_annotations_ static void VmmpHandleEptViolation(
    GuestContext *guest_context) {
  HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
  auto processor_data = guest_context->stack->processor_data;
  EptHandleEptViolation(processor_data->ept_data);
}

// EXIT_REASON_EPT_MISCONFIG
// 处理器虚拟化技术p427
_Use_decl_annotations_ static void VmmpHandleEptMisconfig(
    GuestContext *guest_context) {
  UNREFERENCED_PARAMETER(guest_context);

  const auto fault_address = UtilVmRead(VmcsField::kGuestPhysicalAddress);
  const auto ept_pt_entry = EptGetEptPtEntry(
      guest_context->stack->processor_data->ept_data, fault_address);
  HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kEptMisconfigVmExit,
                                 fault_address,
                                 reinterpret_cast<ULONG_PTR>(ept_pt_entry), 0);
}

// Selects a register to be used based on the index
_Use_decl_annotations_ static ULONG_PTR *VmmpSelectRegister(
    ULONG index, GuestContext *guest_context) {
  ULONG_PTR *register_used = nullptr;
  // clang-format off
  switch (index) {
    case 0: register_used = &guest_context->gp_regs->ax; break;
    case 1: register_used = &guest_context->gp_regs->cx; break;
    case 2: register_used = &guest_context->gp_regs->dx; break;
    case 3: register_used = &guest_context->gp_regs->bx; break;
    case 4: register_used = &guest_context->gp_regs->sp; break;
    case 5: register_used = &guest_context->gp_regs->bp; break;
    case 6: register_used = &guest_context->gp_regs->si; break;
    case 7: register_used = &guest_context->gp_regs->di; break;
#if defined(_AMD64_)
    case 8: register_used = &guest_context->gp_regs->r8; break;
    case 9: register_used = &guest_context->gp_regs->r9; break;
    case 10: register_used = &guest_context->gp_regs->r10; break;
    case 11: register_used = &guest_context->gp_regs->r11; break;
    case 12: register_used = &guest_context->gp_regs->r12; break;
    case 13: register_used = &guest_context->gp_regs->r13; break;
    case 14: register_used = &guest_context->gp_regs->r14; break;
    case 15: register_used = &guest_context->gp_regs->r15; break;
#endif
    default: HYPERPLATFORM_COMMON_DBG_BREAK(); break;
  }
  // clang-format on
  return register_used;
}

// Dumps guest state VMCS fields
/*_Use_decl_annotations_*/ static void VmmpDumpGuestState() {
  // clang-format off
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest EsSelector   = %016Ix", UtilVmRead(VmcsField::kGuestEsSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest CsSelector   = %016Ix", UtilVmRead(VmcsField::kGuestCsSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SsSelector   = %016Ix", UtilVmRead(VmcsField::kGuestSsSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest DsSelector   = %016Ix", UtilVmRead(VmcsField::kGuestDsSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest FsSelector   = %016Ix", UtilVmRead(VmcsField::kGuestFsSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest GsSelector   = %016Ix", UtilVmRead(VmcsField::kGuestGsSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest LdtrSelector = %016Ix", UtilVmRead(VmcsField::kGuestLdtrSelector));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest TrSelector   = %016Ix", UtilVmRead(VmcsField::kGuestTrSelector));

    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Ia32Debugctl = %016llx", UtilVmRead64(VmcsField::kGuestIa32Debugctl));

    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest EsLimit      = %016Ix", UtilVmRead(VmcsField::kGuestEsLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest CsLimit      = %016Ix", UtilVmRead(VmcsField::kGuestCsLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SsLimit      = %016Ix", UtilVmRead(VmcsField::kGuestSsLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest DsLimit      = %016Ix", UtilVmRead(VmcsField::kGuestDsLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest FsLimit      = %016Ix", UtilVmRead(VmcsField::kGuestFsLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest GsLimit      = %016Ix", UtilVmRead(VmcsField::kGuestGsLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest LdtrLimit    = %016Ix", UtilVmRead(VmcsField::kGuestLdtrLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest TrLimit      = %016Ix", UtilVmRead(VmcsField::kGuestTrLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest GdtrLimit    = %016Ix", UtilVmRead(VmcsField::kGuestGdtrLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest IdtrLimit    = %016Ix", UtilVmRead(VmcsField::kGuestIdtrLimit));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest EsArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestEsArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest CsArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestCsArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SsArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestSsArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest DsArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestDsArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest FsArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestFsArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest GsArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestGsArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest LdtrArBytes  = %016Ix", UtilVmRead(VmcsField::kGuestLdtrArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest TrArBytes    = %016Ix", UtilVmRead(VmcsField::kGuestTrArBytes));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SysenterCs   = %016Ix", UtilVmRead(VmcsField::kGuestSysenterCs));

    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Cr0          = %016Ix", UtilVmRead(VmcsField::kGuestCr0));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Cr3          = %016Ix", UtilVmRead(VmcsField::kGuestCr3));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Cr4          = %016Ix", UtilVmRead(VmcsField::kGuestCr4));

    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest EsBase       = %016Ix", UtilVmRead(VmcsField::kGuestEsBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest CsBase       = %016Ix", UtilVmRead(VmcsField::kGuestCsBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SsBase       = %016Ix", UtilVmRead(VmcsField::kGuestSsBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest DsBase       = %016Ix", UtilVmRead(VmcsField::kGuestDsBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest FsBase       = %016Ix", UtilVmRead(VmcsField::kGuestFsBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest GsBase       = %016Ix", UtilVmRead(VmcsField::kGuestGsBase));

    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest LdtrBase     = %016Ix", UtilVmRead(VmcsField::kGuestLdtrBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest TrBase       = %016Ix", UtilVmRead(VmcsField::kGuestTrBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest GdtrBase     = %016Ix", UtilVmRead(VmcsField::kGuestGdtrBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest IdtrBase     = %016Ix", UtilVmRead(VmcsField::kGuestIdtrBase));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Dr7          = %016Ix", UtilVmRead(VmcsField::kGuestDr7));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Rsp          = %016Ix", UtilVmRead(VmcsField::kGuestRsp));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Rip          = %016Ix", UtilVmRead(VmcsField::kGuestRip));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest Rflags       = %016Ix", UtilVmRead(VmcsField::kGuestRflags));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SysenterEsp  = %016Ix", UtilVmRead(VmcsField::kGuestSysenterEsp));
    HYPERPLATFORM_LOG_DEBUG_SAFE("Guest SysenterEip  = %016Ix", UtilVmRead(VmcsField::kGuestSysenterEip));
  // clang-format on
}

// Advances guest's IP to the next instruction
_Use_decl_annotations_ static void VmmpAdjustGuestInstructionPointer(
    GuestContext *guest_context) {

  //让guest执行下一条指令
  const auto exit_inst_length = UtilVmRead(VmcsField::kVmExitInstructionLen);
  UtilVmWrite(VmcsField::kGuestRip, guest_context->ip + exit_inst_length);

  //https://howtohypervise.blogspot.com/2019/01/a-common-missight-in-most-hypervisors.html
  //常规事件注入？ 悬挂异常？
  // Inject #DB if TF is set
#if 1
  if (guest_context->flag_reg.fields.tf) {
#if 1
    VmmpInjectInterruption(InterruptionType::kHardwareException,
                           InterruptionVector::kDebugException, false, 0);
    UtilVmWrite(VmcsField::kVmEntryInstructionLen, exit_inst_length);
#endif
#if 0 
    //
    //测试 pendingException为0
    //
    auto pendingException = (pending_debug_exception)UtilVmRead64(VmcsField::kGuestPendingDbgExceptions);
    HYPERPLATFORM_COMMON_DBG_BREAK();
#endif
  }
#endif 
}

// Handle VMRESUME or VMXOFF failure. Fatal error.
_Use_decl_annotations_ void __stdcall VmmVmxFailureHandler(
    AllRegisters *all_regs) {
  const auto guest_ip = UtilVmRead(VmcsField::kGuestRip);
  // See: VM-Instruction Error Numbers
  const auto vmx_error = (all_regs->flags.fields.zf)
                             ? UtilVmRead(VmcsField::kVmInstructionError)
                             : 0;
  HYPERPLATFORM_COMMON_BUG_CHECK(
      HyperPlatformBugCheck::kCriticalVmxInstructionFailure, vmx_error,
      guest_ip, 0);
}

// Indicates successful VMCALL
_Use_decl_annotations_ static void VmmpIndicateSuccessfulVmcall(
    GuestContext *guest_context) {
  // See: CONVENTIONS
  guest_context->flag_reg.fields.cf = false;
  guest_context->flag_reg.fields.pf = false;
  guest_context->flag_reg.fields.af = false;
  guest_context->flag_reg.fields.zf = false;
  guest_context->flag_reg.fields.sf = false;
  guest_context->flag_reg.fields.of = false;
  guest_context->flag_reg.fields.cf = false;
  guest_context->flag_reg.fields.zf = false;
  UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);
  VmmpAdjustGuestInstructionPointer(guest_context);
}

// Indicates unsuccessful VMCALL
_Use_decl_annotations_ static void VmmpIndicateUnsuccessfulVmcall(
    GuestContext *guest_context) {
  UNREFERENCED_PARAMETER(guest_context);

  VmmpInjectInterruption(InterruptionType::kHardwareException,
                         InterruptionVector::kInvalidOpcodeException, false, 0);
  const auto exit_inst_length = UtilVmRead(VmcsField::kVmExitInstructionLen);
  UtilVmWrite(VmcsField::kVmEntryInstructionLen, exit_inst_length);
}

// Handles an unloading request
_Use_decl_annotations_ static void VmmpHandleVmCallTermination(
    GuestContext *guest_context, void *context) {
  // The processor sets ffff to limits of IDT and GDT when VM-exit occurred.
  // It is not correct value but fine to ignore since vmresume loads correct
  // values from VMCS. But here, we are going to skip vmresume and simply
  // return to where VMCALL is executed. It results in keeping those broken
  // values and ends up with bug check 109, so we should fix them manually.
  const auto gdt_limit = UtilVmRead(VmcsField::kGuestGdtrLimit);
  const auto gdt_base = UtilVmRead(VmcsField::kGuestGdtrBase);
  const auto idt_limit = UtilVmRead(VmcsField::kGuestIdtrLimit);
  const auto idt_base = UtilVmRead(VmcsField::kGuestIdtrBase);
  Gdtr gdtr = {static_cast<USHORT>(gdt_limit), gdt_base};
  Idtr idtr = {static_cast<USHORT>(idt_limit), idt_base};
  __lgdt(&gdtr);
  __lidt(&idtr);

  // Store an address of the management structure to the context parameter
  const auto result_ptr = static_cast<ProcessorData **>(context);
  *result_ptr = guest_context->stack->processor_data;
  HYPERPLATFORM_LOG_DEBUG_SAFE("Context at %p %p", context,
                               guest_context->stack->processor_data);

  // Set rip to the next instruction of VMCALL
  const auto exit_instruction_length =
      UtilVmRead(VmcsField::kVmExitInstructionLen);
  const auto return_address = guest_context->ip + exit_instruction_length;

  // Since the flag register is overwritten after VMXOFF, we should manually
  // indicates that VMCALL was successful by clearing those flags.
  // See: CONVENTIONS
  guest_context->flag_reg.fields.cf = false;
  guest_context->flag_reg.fields.pf = false;
  guest_context->flag_reg.fields.af = false;
  guest_context->flag_reg.fields.zf = false;
  guest_context->flag_reg.fields.sf = false;
  guest_context->flag_reg.fields.of = false;
  guest_context->flag_reg.fields.cf = false;
  guest_context->flag_reg.fields.zf = false;

  // Set registers used after VMXOFF to recover the context. Volatile
  // registers must be used because those changes are reflected to the
  // guest's context after VMXOFF.
  guest_context->gp_regs->cx = return_address;
  guest_context->gp_regs->dx = guest_context->gp_regs->sp;
  guest_context->gp_regs->ax = guest_context->flag_reg.all;
  guest_context->vm_continue = false;
}

// Returns guest's CPL
/*_Use_decl_annotations_*/ static UCHAR VmmpGetGuestCpl() {
  VmxRegmentDescriptorAccessRight ar = {
      static_cast<unsigned int>(UtilVmRead(VmcsField::kGuestSsArBytes))};
  return ar.fields.dpl;
}

//处理器虚拟化技术p214
// Injects interruption to a guest
_Use_decl_annotations_ static void VmmpInjectInterruption(
    InterruptionType interruption_type, InterruptionVector vector,
    bool deliver_error_code, ULONG32 error_code) {
  VmEntryInterruptionInformationField inject = {};
  inject.fields.valid = true;
  inject.fields.interruption_type = static_cast<ULONG32>(interruption_type);
  inject.fields.vector = static_cast<ULONG32>(vector);
  inject.fields.deliver_error_code = deliver_error_code;
  UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);

  if (deliver_error_code) {
    UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, error_code);
  }
}

// Returns a kernel CR3 value of the current process;
/*_Use_decl_annotations_*/ static ULONG_PTR VmmpGetKernelCr3() {
  ULONG_PTR guest_cr3 = 0;
  static const long kDirectoryTableBaseOffset = IsX64() ? 0x28 : 0x18;
  if (IsX64()) {
    // On x64, assume it is an user-mode CR3 when the lowest bit is set. If so,
    // get CR3 from _KPROCESS::DirectoryTableBase.
    guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
    if (guest_cr3 & 1) {
      const auto process = reinterpret_cast<PUCHAR>(PsGetCurrentProcess());
      guest_cr3 =
          *reinterpret_cast<PULONG_PTR>(process + kDirectoryTableBaseOffset);
    }
  } else {
    // On x86, there is no easy way to tell whether the CR3 taken from VMCS is
    // a user-mode CR3 or kernel-mode CR3 by only looking at the value.
    // Therefore, we simply use _KPROCESS::DirectoryTableBase always.
    const auto process = reinterpret_cast<PUCHAR>(PsGetCurrentProcess());
    guest_cr3 =
        *reinterpret_cast<PULONG_PTR>(process + kDirectoryTableBaseOffset);
  }
  return guest_cr3;
}

}  // extern "C"
