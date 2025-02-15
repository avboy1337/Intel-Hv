// Copyright (c) 2015-2019, Satoshi Tanda. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements an entry point of the driver.

#ifndef POOL_NX_OPTIN
#define POOL_NX_OPTIN 1
#endif
#include "include/write_protect.h"
#include "driver.h"
#include "common.h"
#include "global_object.h"
#include "hotplug_callback.h"
#include "log.h"
#include "power_callback.h"
#include "util.h"
#include "vm.h"
#include "performance.h"
#include "systemcall.h"
#include "settings.h"
#include"include/global.hpp"
#include"service_hook.h"
#include"device.h"
#include"window.h"

extern "C"
{
#include "kernel-hook/khook/khook/hk.h"
}



//
//实现于systemcall.cpp
//
extern NTSTATUS InitSystemVar();
extern void DoSystemCallHook();

extern NTSTATUS HookStatus;
extern fpSystemCall SystemCallFake;
extern char SystemCallRecoverCode[15];
 
extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

DRIVER_INITIALIZE DriverEntry;

static DRIVER_UNLOAD DriverpDriverUnload;

_IRQL_requires_max_(PASSIVE_LEVEL) bool DriverpIsSuppoetedOS();

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, DriverpDriverUnload)
#pragma alloc_text(INIT, DriverpIsSuppoetedOS)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// A driver entry point
_Use_decl_annotations_ NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                            PUNICODE_STRING registry_path) {
  UNREFERENCED_PARAMETER(registry_path);
  PAGED_CODE()

  static const wchar_t kLogFilePath[] = L"\\SystemRoot\\HyperPlatform.log";
  static const auto kLogLevel =
      (IsReleaseBuild()) ? kLogPutLevelInfo | kLogOptDisableFunctionName
                         : kLogPutLevelDebug | kLogOptDisableFunctionName;

  auto status = STATUS_UNSUCCESSFUL;
  driver_object->DriverUnload = DriverpDriverUnload;

  status = InitSystemVar();
  if (!NT_SUCCESS(status))
  {
      return STATUS_UNSUCCESSFUL;
  }

  _CRT_INIT();


  status = HyperInitDeviceAll(driver_object);

  if (!NT_SUCCESS(status))
  {
      return STATUS_UNSUCCESSFUL;
  }


#ifdef HOOK_SYSCALL 
  InitUserSystemCallHandler(SystemCallLog);

  //是否要开启KiSystemCall64的hook
  DoSystemCallHook();

#endif

#ifdef SERVICE_HOOK
  
  //hook NtOpenProcess
  AddServiceHook(UtilGetSystemProcAddress(L"NtOpenProcess"), DetourNtOpenProcess,(PVOID*)&OriNtOpenProcess);
  //hook NtCreateFile
  AddServiceHook(UtilGetSystemProcAddress(L"NtCreateFile"), DetourNtCreateFile, (PVOID*)&OriNtCreateFile);
  //hook NtWriteVirtualMemory
  AddServiceHook(
      PVOID(KernelBase + OffsetNtWriteVirtualMemory), 
      DetourNtWriteVirtualMemory, 
      (PVOID*)&OriNtWriteVirtualMemory
  );
  //hook NtCreateThreadEx
  AddServiceHook(
      PVOID(KernelBase + OffsetNtCreateThreadEx),
      DetourNtCreateThreadEx,
      (PVOID*)&OriNtCreateThreadEx);

  //hook NtAllocateVirtualMemory
  AddServiceHook(UtilGetSystemProcAddress(L"NtAllocateVirtualMemory"), DetourNtAllocateVirtualMemory,
      (PVOID*)&OriNtAllocateVirtualMemory);

  //hook NtCreateThread
  AddServiceHook(
      PVOID(KernelBase + OffsetNtCreateThread),
      DetourNtCreateThread,
      (PVOID*)&OriNtCreateThread);

  AddServiceHook(PVOID(KernelBase + OffsetNtDeviceIoControlFile),
      DetourNtDeviceIoControlFile, (PVOID*)&OriNtDeviceIoControlFile);


#ifdef HIDE_WINDOW
  AddServiceHook(PVOID(Win32kfullBase + OffsetNtUserFindWindowEx),
      DetourNtUserFindWindowEx,
      (PVOID*)&OriNtUserFindWindowEx);
#endif
 
 
#endif 

#ifdef HIDE_WINDOW
  Window::Init();
  //AttackWindowTable();
#endif // HIDE_WINDOW





  //
  //便于测试,屏蔽掉虚拟化的功能
  //有些bug需要关虚拟化后复现蓝屏才能发现
  //
#if 0
  return STATUS_SUCCESS;
#endif

#if 0
  HYPERPLATFORM_COMMON_DBG_BREAK();
#endif

  //https://docs.microsoft.com/en-us/windows-hardware/drivers/kernel/single-binary-opt-in-pool-nx-optin
  // Request NX Non-Paged Pool when available
  ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

  // Initialize log functions
  bool need_reinitialization = false;
  status = LogInitialization(kLogLevel, kLogFilePath);
  if (status == STATUS_REINITIALIZATION_NEEDED) {
    need_reinitialization = true;
  } else if (!NT_SUCCESS(status)) {
    return status;
  }

  // Test if the system is supported
  if (!DriverpIsSuppoetedOS()) {
    LogTermination();
    return STATUS_CANCELLED;
  }

  // Initialize global variables
  // 调用全局类的构造函数
  //status = GlobalObjectInitialization();
  //if (!NT_SUCCESS(status)) {
    //LogTermination();
    //return status;
  //}

  // Initialize perf functions
  status = PerfInitialization();
  if (!NT_SUCCESS(status)) {
    //GlobalObjectTermination();
    _CRT_UNLOAD();
    LogTermination();
    return status;
  }

  // Initialize utility functions
  status = UtilInitialization(driver_object);
  if (!NT_SUCCESS(status)) {
    PerfTermination();
    //GlobalObjectTermination();
    _CRT_UNLOAD();
    LogTermination();
    return status;
  }

  // Initialize power callback
  status = PowerCallbackInitialization();
  if (!NT_SUCCESS(status)) {
    UtilTermination();
    PerfTermination();
    //GlobalObjectTermination();
    _CRT_UNLOAD();
    LogTermination();
    return status;
  }

  // Initialize hot-plug callback
  status = HotplugCallbackInitialization();
  if (!NT_SUCCESS(status)) {
    PowerCallbackTermination();
    UtilTermination();
    PerfTermination();
    //GlobalObjectTermination();
    _CRT_UNLOAD();
    LogTermination();
    return status;
  }

  // Virtualize all processors
  status = VmInitialization();
  if (!NT_SUCCESS(status)) {
    HotplugCallbackTermination();
    PowerCallbackTermination();
    UtilTermination();
    PerfTermination();
    //GlobalObjectTermination();
    _CRT_UNLOAD();
    LogTermination();
    return status;
  }

  // Register re-initialization for the log functions if needed
  if (need_reinitialization) {
    LogRegisterReinitialization(driver_object);
  }

  HYPERPLATFORM_LOG_INFO("The VMM has been installed.");

  





  return status;
}

// Unload handler
_Use_decl_annotations_ static void DriverpDriverUnload(
    PDRIVER_OBJECT driver_object) {
  UNREFERENCED_PARAMETER(driver_object);
  PAGED_CODE()
#if 0
  HYPERPLATFORM_COMMON_DBG_BREAK();
#endif
  VmTermination();
  HotplugCallbackTermination();
  PowerCallbackTermination();
  UtilTermination();
  PerfTermination();
  //GlobalObjectTermination();
  LogTermination();
#ifdef HOOK_SYSCALL
  auto irql = WPOFFx64();
  memcpy((PVOID)KiSystemServiceStart, SystemCallRecoverCode, sizeof(SystemCallRecoverCode));
  WPONx64(irql);
  if (SystemCallFake.fp.PageContent)
      ExFreePool(SystemCallFake.fp.PageContent);
#endif

#ifdef SERVICE_HOOK
  RemoveServiceHook();
#endif

  HyperDestroyDeviceAll(driver_object);

}

// Test if the system is one of supported OS versions
_Use_decl_annotations_ bool DriverpIsSuppoetedOS() {
  PAGED_CODE()

  RTL_OSVERSIONINFOW os_version = {};
  auto status = RtlGetVersion(&os_version);
  if (!NT_SUCCESS(status)) {
    return false;
  }
  if (os_version.dwMajorVersion != 6 && os_version.dwMajorVersion != 10) {
    return false;
  }
  // 4-gigabyte tuning (4GT) should not be enabled
  if (!IsX64() && 
      reinterpret_cast<ULONG_PTR>(MmSystemRangeStart) != 0x80000000) {
    return false;
  }
  return true;
}

}  // extern "C"
