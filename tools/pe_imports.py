"""Read-only PE inspection: sections, imports, and threading/timing API usage.

Usage: python pe_imports.py <path-to-exe>
The target file is opened read-only and never written.
"""
import sys
import pefile

INTERESTING = {
    # thread creation / scheduling
    "CreateThread", "_beginthreadex", "_beginthread", "ExitThread", "SetThreadAffinityMask",
    "SetProcessAffinityMask", "SetThreadIdealProcessor", "SetThreadIdealProcessorEx",
    "SetThreadPriority", "SetPriorityClass", "SetThreadGroupAffinity", "SetThreadSelectedCpuSets",
    "SetProcessDefaultCpuSets", "GetSystemInfo", "GetNativeSystemInfo",
    "GetLogicalProcessorInformation", "GetLogicalProcessorInformationEx", "GetActiveProcessorCount",
    "GetMaximumProcessorCount", "GetCurrentProcessorNumber", "GetCurrentProcessorNumberEx",
    "SetThreadDescription", "SetProcessPriorityBoost", "SetThreadPriorityBoost",
    # waiting / yielding
    "Sleep", "SleepEx", "SwitchToThread", "WaitForSingleObject", "WaitForSingleObjectEx",
    "WaitForMultipleObjects", "WaitForMultipleObjectsEx", "SignalObjectAndWait",
    "WaitOnAddress", "WakeByAddressSingle", "WakeByAddressAll",
    "SleepConditionVariableCS", "SleepConditionVariableSRW", "WakeConditionVariable",
    "WakeAllConditionVariable", "CreateEventA", "CreateEventW", "CreateEventExW", "SetEvent",
    "ResetEvent", "CreateSemaphoreA", "CreateSemaphoreW", "CreateSemaphoreExW", "ReleaseSemaphore",
    "CreateWaitableTimerA", "CreateWaitableTimerW", "CreateWaitableTimerExW", "SetWaitableTimer",
    "SetWaitableTimerEx", "NtDelayExecution", "NtSetTimerResolution", "ZwSetTimerResolution",
    # locks
    "EnterCriticalSection", "TryEnterCriticalSection", "InitializeCriticalSectionAndSpinCount",
    "InitializeCriticalSectionEx", "AcquireSRWLockExclusive", "AcquireSRWLockShared",
    # timing
    "QueryPerformanceCounter", "QueryPerformanceFrequency", "timeBeginPeriod", "timeEndPeriod",
    "timeGetTime", "GetTickCount", "GetTickCount64", "GetSystemTimeAsFileTime",
    "GetSystemTimePreciseAsFileTime", "timeGetDevCaps",
    # thread pools
    "CreateThreadpoolWork", "SubmitThreadpoolWork", "QueueUserWorkItem", "TrySubmitThreadpoolCallback",
    "CreateIoCompletionPort", "GetQueuedCompletionStatus", "PostQueuedCompletionStatus",
    # rendering
    "D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain", "CreateDXGIFactory", "CreateDXGIFactory1",
    "CreateDXGIFactory2", "D3D12CreateDevice",
}


def main(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
    ])
    print(f"ImageBase      : {pe.OPTIONAL_HEADER.ImageBase:#x}")
    print(f"EntryPoint RVA : {pe.OPTIONAL_HEADER.AddressOfEntryPoint:#x}")
    print(f"Timestamp      : {pe.FILE_HEADER.TimeDateStamp}")
    print(f"Machine        : {pe.FILE_HEADER.Machine:#x}")
    print("\nSections:")
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        print(f"  {name:10} VA={s.VirtualAddress:#010x} VSize={s.Misc_VirtualSize:#010x} "
              f"Raw={s.SizeOfRawData:#010x} chars={s.Characteristics:#x} entropy={s.get_entropy():.2f}")

    print("\nImported DLLs:")
    entries = list(getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])) + \
        list(getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []))
    hits = []
    for entry in entries:
        dll = entry.dll.decode(errors="replace")
        print(f"  {dll} ({len(entry.imports)} functions)")
        for imp in entry.imports:
            name = imp.name.decode(errors="replace") if imp.name else f"ord{imp.ordinal}"
            if name in INTERESTING:
                hits.append((dll, name, imp.address))

    print("\nThreading / timing / render imports (IAT VA):")
    for dll, name, addr in sorted(hits, key=lambda h: (h[0].lower(), h[1])):
        print(f"  {dll:28} {name:40} IAT={addr:#x}")


if __name__ == "__main__":
    main(sys.argv[1])
