// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <iterator>

#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/mutex.h"
#include "core/hle/kernel/object_address_table.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_wrap.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/lock.h"
#include "core/hle/result.h"
#include "core/hle/service/service.h"

namespace Kernel {

/// Set the process heap to a given Size. It can both extend and shrink the heap.
static ResultCode SetHeapSize(VAddr* heap_addr, u64 heap_size) {
    NGLOG_TRACE(Kernel_SVC, "called, heap_size={:#X}", heap_size);
    auto& process = *Core::CurrentProcess();
    CASCADE_RESULT(*heap_addr,
                   process.HeapAllocate(Memory::HEAP_VADDR, heap_size, VMAPermission::ReadWrite));
    return RESULT_SUCCESS;
}

static ResultCode SetMemoryAttribute(VAddr addr, u64 size, u32 state0, u32 state1) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called, addr={:#X}", addr);
    return RESULT_SUCCESS;
}

/// Maps a memory range into a different range.
static ResultCode MapMemory(VAddr dst_addr, VAddr src_addr, u64 size) {
    NGLOG_TRACE(Kernel_SVC, "called, dst_addr={:#X}, src_addr={:#X}, size={:#X}", dst_addr,
                src_addr, size);
    return Core::CurrentProcess()->MirrorMemory(dst_addr, src_addr, size);
}

/// Unmaps a region that was previously mapped with svcMapMemory
static ResultCode UnmapMemory(VAddr dst_addr, VAddr src_addr, u64 size) {
    NGLOG_TRACE(Kernel_SVC, "called, dst_addr={:#X}, src_addr={:#X}, size={:#X}", dst_addr,
                src_addr, size);
    return Core::CurrentProcess()->UnmapMemory(dst_addr, src_addr, size);
}

/// Connect to an OS service given the port name, returns the handle to the port to out
static ResultCode ConnectToNamedPort(Handle* out_handle, VAddr port_name_address) {
    if (!Memory::IsValidVirtualAddress(port_name_address))
        return ERR_NOT_FOUND;

    static constexpr std::size_t PortNameMaxLength = 11;
    // Read 1 char beyond the max allowed port name to detect names that are too long.
    std::string port_name = Memory::ReadCString(port_name_address, PortNameMaxLength + 1);
    if (port_name.size() > PortNameMaxLength)
        return ERR_PORT_NAME_TOO_LONG;

    NGLOG_TRACE(Kernel_SVC, "called port_name={}", port_name);

    auto it = Service::g_kernel_named_ports.find(port_name);
    if (it == Service::g_kernel_named_ports.end()) {
        NGLOG_WARNING(Kernel_SVC, "tried to connect to unknown port: {}", port_name);
        return ERR_NOT_FOUND;
    }

    auto client_port = it->second;

    SharedPtr<ClientSession> client_session;
    CASCADE_RESULT(client_session, client_port->Connect());

    // Return the client session
    CASCADE_RESULT(*out_handle, g_handle_table.Create(client_session));
    return RESULT_SUCCESS;
}

/// Makes a blocking IPC call to an OS service.
static ResultCode SendSyncRequest(Handle handle) {
    SharedPtr<ClientSession> session = g_handle_table.Get<ClientSession>(handle);
    if (!session) {
        NGLOG_ERROR(Kernel_SVC, "called with invalid handle={:#010X}", handle);
        return ERR_INVALID_HANDLE;
    }

    NGLOG_TRACE(Kernel_SVC, "called handle={:#010X}({})", handle, session->GetName());

    Core::System::GetInstance().PrepareReschedule();

    // TODO(Subv): svcSendSyncRequest should put the caller thread to sleep while the server
    // responds and cause a reschedule.
    return session->SendSyncRequest(GetCurrentThread());
}

/// Get the ID for the specified thread.
static ResultCode GetThreadId(u32* thread_id, Handle thread_handle) {
    NGLOG_TRACE(Kernel_SVC, "called thread={:#010X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    *thread_id = thread->GetThreadId();
    return RESULT_SUCCESS;
}

/// Get the ID of the specified process
static ResultCode GetProcessId(u32* process_id, Handle process_handle) {
    NGLOG_TRACE(Kernel_SVC, "called process={:#010X}", process_handle);

    const SharedPtr<Process> process = g_handle_table.Get<Process>(process_handle);
    if (!process) {
        return ERR_INVALID_HANDLE;
    }

    *process_id = process->process_id;
    return RESULT_SUCCESS;
}

/// Default thread wakeup callback for WaitSynchronization
static bool DefaultThreadWakeupCallback(ThreadWakeupReason reason, SharedPtr<Thread> thread,
                                        SharedPtr<WaitObject> object, size_t index) {
    ASSERT(thread->status == THREADSTATUS_WAIT_SYNCH_ANY);

    if (reason == ThreadWakeupReason::Timeout) {
        thread->SetWaitSynchronizationResult(RESULT_TIMEOUT);
        return true;
    }

    ASSERT(reason == ThreadWakeupReason::Signal);
    thread->SetWaitSynchronizationResult(RESULT_SUCCESS);
    thread->SetWaitSynchronizationOutput(static_cast<u32>(index));
    return true;
};

/// Wait for a kernel object to synchronize, timeout after the specified nanoseconds
static ResultCode WaitSynchronization1(
    SharedPtr<WaitObject> object, Thread* thread, s64 nano_seconds = -1,
    std::function<Thread::WakeupCallback> wakeup_callback = DefaultThreadWakeupCallback) {

    if (!object) {
        return ERR_INVALID_HANDLE;
    }

    if (object->ShouldWait(thread)) {
        if (nano_seconds == 0) {
            return RESULT_TIMEOUT;
        }

        thread->wait_objects = {object};
        object->AddWaitingThread(thread);
        thread->status = THREADSTATUS_WAIT_SYNCH_ANY;

        // Create an event to wake the thread up after the specified nanosecond delay has passed
        thread->WakeAfterDelay(nano_seconds);
        thread->wakeup_callback = wakeup_callback;

        Core::System::GetInstance().PrepareReschedule();
    } else {
        object->Acquire(thread);
    }

    return RESULT_SUCCESS;
}

/// Wait for the given handles to synchronize, timeout after the specified nanoseconds
static ResultCode WaitSynchronization(Handle* index, VAddr handles_address, u64 handle_count,
                                      s64 nano_seconds) {
    NGLOG_TRACE(Kernel_SVC, "called handles_address={:#X}, handle_count={}, nano_seconds={}",
                handles_address, handle_count, nano_seconds);

    if (!Memory::IsValidVirtualAddress(handles_address))
        return ERR_INVALID_POINTER;

    static constexpr u64 MaxHandles = 0x40;

    if (handle_count > MaxHandles)
        return ResultCode(ErrorModule::Kernel, ErrCodes::TooLarge);

    auto thread = GetCurrentThread();

    using ObjectPtr = SharedPtr<WaitObject>;
    std::vector<ObjectPtr> objects(handle_count);

    for (int i = 0; i < handle_count; ++i) {
        Handle handle = Memory::Read32(handles_address + i * sizeof(Handle));
        auto object = g_handle_table.Get<WaitObject>(handle);
        if (object == nullptr)
            return ERR_INVALID_HANDLE;
        objects[i] = object;
    }

    // Find the first object that is acquirable in the provided list of objects
    auto itr = std::find_if(objects.begin(), objects.end(), [thread](const ObjectPtr& object) {
        return !object->ShouldWait(thread);
    });

    if (itr != objects.end()) {
        // We found a ready object, acquire it and set the result value
        WaitObject* object = itr->get();
        object->Acquire(thread);
        *index = static_cast<s32>(std::distance(objects.begin(), itr));
        return RESULT_SUCCESS;
    }

    // No objects were ready to be acquired, prepare to suspend the thread.

    // If a timeout value of 0 was provided, just return the Timeout error code instead of
    // suspending the thread.
    if (nano_seconds == 0)
        return RESULT_TIMEOUT;

    for (auto& object : objects)
        object->AddWaitingThread(thread);

    thread->wait_objects = std::move(objects);
    thread->status = THREADSTATUS_WAIT_SYNCH_ANY;

    // Create an event to wake the thread up after the specified nanosecond delay has passed
    thread->WakeAfterDelay(nano_seconds);
    thread->wakeup_callback = DefaultThreadWakeupCallback;

    Core::System::GetInstance().PrepareReschedule();

    return RESULT_TIMEOUT;
}

/// Resumes a thread waiting on WaitSynchronization
static ResultCode CancelSynchronization(Handle thread_handle) {
    NGLOG_TRACE(Kernel_SVC, "called thread={:#X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    ASSERT(thread->status == THREADSTATUS_WAIT_SYNCH_ANY);
    thread->SetWaitSynchronizationResult(
        ResultCode(ErrorModule::Kernel, ErrCodes::SynchronizationCanceled));
    thread->ResumeFromWait();
    return RESULT_SUCCESS;
}

/// Attempts to locks a mutex, creating it if it does not already exist
static ResultCode ArbitrateLock(Handle holding_thread_handle, VAddr mutex_addr,
                                Handle requesting_thread_handle) {
    NGLOG_TRACE(Kernel_SVC,
                "called holding_thread_handle={:#010X}, mutex_addr={:#X}, "
                "requesting_current_thread_handle={:#010X}",
                holding_thread_handle, mutex_addr, requesting_thread_handle);

    return Mutex::TryAcquire(mutex_addr, holding_thread_handle, requesting_thread_handle);
}

/// Unlock a mutex
static ResultCode ArbitrateUnlock(VAddr mutex_addr) {
    NGLOG_TRACE(Kernel_SVC, "called mutex_addr={:#X}", mutex_addr);

    return Mutex::Release(mutex_addr);
}

/// Break program execution
static void Break(u64 unk_0, u64 unk_1, u64 unk_2) {
    NGLOG_CRITICAL(Debug_Emulated, "Emulated program broke execution!");
    ASSERT(false);
}

/// Used to output a message on a debug hardware unit - does nothing on a retail unit
static void OutputDebugString(VAddr address, s32 len) {
    std::string str(len, '\0');
    Memory::ReadBlock(address, str.data(), str.size());
    NGLOG_DEBUG(Debug_Emulated, "{}", str);
}

/// Gets system/memory information for the current process
static ResultCode GetInfo(u64* result, u64 info_id, u64 handle, u64 info_sub_id) {
    NGLOG_TRACE(Kernel_SVC, "called info_id={:#X}, info_sub_id={:#X}, handle={:#010X}", info_id,
                info_sub_id, handle);

    auto& vm_manager = Core::CurrentProcess()->vm_manager;

    switch (static_cast<GetInfoType>(info_id)) {
    case GetInfoType::AllowedCpuIdBitmask:
        *result = Core::CurrentProcess()->allowed_processor_mask;
        break;
    case GetInfoType::AllowedThreadPrioBitmask:
        *result = Core::CurrentProcess()->allowed_thread_priority_mask;
        break;
    case GetInfoType::MapRegionBaseAddr:
        *result = Memory::MAP_REGION_VADDR;
        break;
    case GetInfoType::MapRegionSize:
        *result = Memory::MAP_REGION_SIZE;
        break;
    case GetInfoType::HeapRegionBaseAddr:
        *result = Memory::HEAP_VADDR;
        break;
    case GetInfoType::HeapRegionSize:
        *result = Memory::HEAP_SIZE;
        break;
    case GetInfoType::TotalMemoryUsage:
        *result = vm_manager.GetTotalMemoryUsage();
        break;
    case GetInfoType::TotalHeapUsage:
        *result = vm_manager.GetTotalHeapUsage();
        break;
    case GetInfoType::IsCurrentProcessBeingDebugged:
        *result = 0;
        break;
    case GetInfoType::RandomEntropy:
        *result = 0;
        break;
    case GetInfoType::AddressSpaceBaseAddr:
        *result = vm_manager.GetAddressSpaceBaseAddr();
        break;
    case GetInfoType::AddressSpaceSize:
        *result = vm_manager.GetAddressSpaceSize();
        break;
    case GetInfoType::NewMapRegionBaseAddr:
        *result = Memory::NEW_MAP_REGION_VADDR;
        break;
    case GetInfoType::NewMapRegionSize:
        *result = Memory::NEW_MAP_REGION_SIZE;
        break;
    case GetInfoType::IsVirtualAddressMemoryEnabled:
        *result = Core::CurrentProcess()->is_virtual_address_memory_enabled;
        break;
    case GetInfoType::TitleId:
        NGLOG_WARNING(Kernel_SVC, "(STUBBED) Attempted to query titleid, returned 0");
        *result = 0;
        break;
    case GetInfoType::PrivilegedProcessId:
        NGLOG_WARNING(Kernel_SVC,
                      "(STUBBED) Attempted to query privileged process id bounds, returned 0");
        *result = 0;
        break;
    default:
        UNIMPLEMENTED();
    }

    return RESULT_SUCCESS;
}

/// Sets the thread activity
static ResultCode SetThreadActivity(Handle handle, u32 unknown) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called, handle={:#010X}, unknown={:#010X}", handle,
                  unknown);
    return RESULT_SUCCESS;
}

/// Gets the thread context
static ResultCode GetThreadContext(Handle handle, VAddr addr) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called, handle={:#010X}, addr={:#X}", handle, addr);
    return RESULT_SUCCESS;
}

/// Gets the priority for the specified thread
static ResultCode GetThreadPriority(u32* priority, Handle handle) {
    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(handle);
    if (!thread)
        return ERR_INVALID_HANDLE;

    *priority = thread->GetPriority();
    return RESULT_SUCCESS;
}

/// Sets the priority for the specified thread
static ResultCode SetThreadPriority(Handle handle, u32 priority) {
    if (priority > THREADPRIO_LOWEST) {
        return ERR_OUT_OF_RANGE;
    }

    SharedPtr<Thread> thread = g_handle_table.Get<Thread>(handle);
    if (!thread)
        return ERR_INVALID_HANDLE;

    // Note: The kernel uses the current process's resource limit instead of
    // the one from the thread owner's resource limit.
    SharedPtr<ResourceLimit>& resource_limit = Core::CurrentProcess()->resource_limit;
    if (resource_limit->GetMaxResourceValue(ResourceType::Priority) > priority) {
        return ERR_NOT_AUTHORIZED;
    }

    thread->SetPriority(priority);

    Core::System::GetInstance().PrepareReschedule();
    return RESULT_SUCCESS;
}

/// Get which CPU core is executing the current thread
static u32 GetCurrentProcessorNumber() {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called, defaulting to processor 0");
    return 0;
}

static ResultCode MapSharedMemory(Handle shared_memory_handle, VAddr addr, u64 size,
                                  u32 permissions) {
    NGLOG_TRACE(Kernel_SVC,
                "called, shared_memory_handle={:#X}, addr={:#X}, size={:#X}, permissions={:#010X}",
                shared_memory_handle, addr, size, permissions);

    SharedPtr<SharedMemory> shared_memory = g_handle_table.Get<SharedMemory>(shared_memory_handle);
    if (!shared_memory) {
        return ERR_INVALID_HANDLE;
    }

    MemoryPermission permissions_type = static_cast<MemoryPermission>(permissions);
    switch (permissions_type) {
    case MemoryPermission::Read:
    case MemoryPermission::Write:
    case MemoryPermission::ReadWrite:
    case MemoryPermission::Execute:
    case MemoryPermission::ReadExecute:
    case MemoryPermission::WriteExecute:
    case MemoryPermission::ReadWriteExecute:
    case MemoryPermission::DontCare:
        return shared_memory->Map(Core::CurrentProcess().get(), addr, permissions_type,
                                  MemoryPermission::DontCare);
    default:
        NGLOG_ERROR(Kernel_SVC, "unknown permissions={:#010X}", permissions);
    }

    return RESULT_SUCCESS;
}

static ResultCode UnmapSharedMemory(Handle shared_memory_handle, VAddr addr, u64 size) {
    NGLOG_WARNING(Kernel_SVC, "called, shared_memory_handle={:#010X}, addr={:#X}, size={:#X}",
                  shared_memory_handle, addr, size);

    SharedPtr<SharedMemory> shared_memory = g_handle_table.Get<SharedMemory>(shared_memory_handle);

    return shared_memory->Unmap(Core::CurrentProcess().get(), addr);
}

/// Query process memory
static ResultCode QueryProcessMemory(MemoryInfo* memory_info, PageInfo* /*page_info*/,
                                     Handle process_handle, u64 addr) {
    SharedPtr<Process> process = g_handle_table.Get<Process>(process_handle);
    if (!process) {
        return ERR_INVALID_HANDLE;
    }
    auto vma = process->vm_manager.FindVMA(addr);
    memory_info->attributes = 0;
    if (vma == Core::CurrentProcess()->vm_manager.vma_map.end()) {
        memory_info->base_address = 0;
        memory_info->permission = static_cast<u32>(VMAPermission::None);
        memory_info->size = 0;
        memory_info->type = static_cast<u32>(MemoryState::Unmapped);
    } else {
        memory_info->base_address = vma->second.base;
        memory_info->permission = static_cast<u32>(vma->second.permissions);
        memory_info->size = vma->second.size;
        memory_info->type = static_cast<u32>(vma->second.meminfo_state);
    }

    NGLOG_TRACE(Kernel_SVC, "called process={:#010X} addr={:X}", process_handle, addr);
    return RESULT_SUCCESS;
}

/// Query memory
static ResultCode QueryMemory(MemoryInfo* memory_info, PageInfo* page_info, VAddr addr) {
    NGLOG_TRACE(Kernel_SVC, "called, addr={:X}", addr);
    return QueryProcessMemory(memory_info, page_info, CurrentProcess, addr);
}

/// Exits the current process
static void ExitProcess() {
    NGLOG_INFO(Kernel_SVC, "Process {} exiting", Core::CurrentProcess()->process_id);

    ASSERT_MSG(Core::CurrentProcess()->status == ProcessStatus::Running,
               "Process has already exited");

    Core::CurrentProcess()->status = ProcessStatus::Exited;

    // Stop all the process threads that are currently waiting for objects.
    auto& thread_list = Core::System::GetInstance().Scheduler().GetThreadList();
    for (auto& thread : thread_list) {
        if (thread->owner_process != Core::CurrentProcess())
            continue;

        if (thread == GetCurrentThread())
            continue;

        // TODO(Subv): When are the other running/ready threads terminated?
        ASSERT_MSG(thread->status == THREADSTATUS_WAIT_SYNCH_ANY ||
                       thread->status == THREADSTATUS_WAIT_SYNCH_ALL,
                   "Exiting processes with non-waiting threads is currently unimplemented");

        thread->Stop();
    }

    // Kill the current thread
    GetCurrentThread()->Stop();

    Core::System::GetInstance().PrepareReschedule();
}

/// Creates a new thread
static ResultCode CreateThread(Handle* out_handle, VAddr entry_point, u64 arg, VAddr stack_top,
                               u32 priority, s32 processor_id) {
    std::string name = Common::StringFromFormat("unknown-%llx", entry_point);

    if (priority > THREADPRIO_LOWEST) {
        return ERR_OUT_OF_RANGE;
    }

    SharedPtr<ResourceLimit>& resource_limit = Core::CurrentProcess()->resource_limit;
    if (resource_limit->GetMaxResourceValue(ResourceType::Priority) > priority) {
        return ERR_NOT_AUTHORIZED;
    }

    if (processor_id == THREADPROCESSORID_DEFAULT) {
        // Set the target CPU to the one specified in the process' exheader.
        processor_id = Core::CurrentProcess()->ideal_processor;
        ASSERT(processor_id != THREADPROCESSORID_DEFAULT);
    }

    switch (processor_id) {
    case THREADPROCESSORID_0:
        break;
    case THREADPROCESSORID_1:
    case THREADPROCESSORID_2:
    case THREADPROCESSORID_3:
        // TODO(bunnei): Implement support for other processor IDs
        NGLOG_ERROR(Kernel_SVC,
                    "Newly created thread must run in another thread ({}), unimplemented.",
                    processor_id);
        break;
    default:
        ASSERT_MSG(false, "Unsupported thread processor ID: {}", processor_id);
        break;
    }

    CASCADE_RESULT(SharedPtr<Thread> thread,
                   Thread::Create(name, entry_point, priority, arg, processor_id, stack_top,
                                  Core::CurrentProcess()));
    CASCADE_RESULT(thread->guest_handle, g_handle_table.Create(thread));
    *out_handle = thread->guest_handle;

    Core::System::GetInstance().PrepareReschedule();

    NGLOG_TRACE(Kernel_SVC,
                "called entrypoint={:#010X} ({}), arg={:#010X}, stacktop={:#010X}, "
                "threadpriority={:#010X}, processorid={:#010X} : created handle={:#010X}",
                entry_point, name, arg, stack_top, priority, processor_id, *out_handle);

    return RESULT_SUCCESS;
}

/// Starts the thread for the provided handle
static ResultCode StartThread(Handle thread_handle) {
    NGLOG_TRACE(Kernel_SVC, "called thread={:#010X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    thread->ResumeFromWait();

    return RESULT_SUCCESS;
}

/// Called when a thread exits
static void ExitThread() {
    NGLOG_TRACE(Kernel_SVC, "called, pc={:#010X}", Core::CPU().GetPC());

    ExitCurrentThread();
    Core::System::GetInstance().PrepareReschedule();
}

/// Sleep the current thread
static void SleepThread(s64 nanoseconds) {
    NGLOG_TRACE(Kernel_SVC, "called nanoseconds={}", nanoseconds);

    // Don't attempt to yield execution if there are no available threads to run,
    // this way we avoid a useless reschedule to the idle thread.
    if (nanoseconds == 0 && !Core::System::GetInstance().Scheduler().HaveReadyThreads())
        return;

    // Sleep current thread and check for next thread to schedule
    WaitCurrentThread_Sleep();

    // Create an event to wake the thread up after the specified nanosecond delay has passed
    GetCurrentThread()->WakeAfterDelay(nanoseconds);

    Core::System::GetInstance().PrepareReschedule();
}

/// Signal process wide key atomic
static ResultCode WaitProcessWideKeyAtomic(VAddr mutex_addr, VAddr condition_variable_addr,
                                           Handle thread_handle, s64 nano_seconds) {
    NGLOG_TRACE(
        Kernel_SVC,
        "called mutex_addr={:X}, condition_variable_addr={:X}, thread_handle={:#010X}, timeout={}",
        mutex_addr, condition_variable_addr, thread_handle, nano_seconds);

    SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    ASSERT(thread);

    CASCADE_CODE(Mutex::Release(mutex_addr));

    SharedPtr<Thread> current_thread = GetCurrentThread();
    current_thread->condvar_wait_address = condition_variable_addr;
    current_thread->mutex_wait_address = mutex_addr;
    current_thread->wait_handle = thread_handle;
    current_thread->status = THREADSTATUS_WAIT_MUTEX;
    current_thread->wakeup_callback = nullptr;

    current_thread->WakeAfterDelay(nano_seconds);

    // Note: Deliberately don't attempt to inherit the lock owner's priority.

    Core::System::GetInstance().PrepareReschedule();
    return RESULT_SUCCESS;
}

/// Signal process wide key
static ResultCode SignalProcessWideKey(VAddr condition_variable_addr, s32 target) {
    NGLOG_TRACE(Kernel_SVC, "called, condition_variable_addr={:#X}, target={:#010X}",
                condition_variable_addr, target);

    u32 processed = 0;
    auto& thread_list = Core::System::GetInstance().Scheduler().GetThreadList();

    for (auto& thread : thread_list) {
        if (thread->condvar_wait_address != condition_variable_addr)
            continue;

        // Only process up to 'target' threads, unless 'target' is -1, in which case process
        // them all.
        if (target != -1 && processed >= target)
            break;

        // If the mutex is not yet acquired, acquire it.
        u32 mutex_val = Memory::Read32(thread->mutex_wait_address);

        if (mutex_val == 0) {
            // We were able to acquire the mutex, resume this thread.
            Memory::Write32(thread->mutex_wait_address, thread->wait_handle);
            ASSERT(thread->status == THREADSTATUS_WAIT_MUTEX);
            thread->ResumeFromWait();

            auto lock_owner = thread->lock_owner;
            if (lock_owner)
                lock_owner->RemoveMutexWaiter(thread);

            thread->lock_owner = nullptr;
            thread->mutex_wait_address = 0;
            thread->condvar_wait_address = 0;
            thread->wait_handle = 0;
        } else {
            // Couldn't acquire the mutex, block the thread.
            Handle owner_handle = static_cast<Handle>(mutex_val & Mutex::MutexOwnerMask);
            auto owner = g_handle_table.Get<Thread>(owner_handle);
            ASSERT(owner);
            ASSERT(thread->status != THREADSTATUS_RUNNING);
            thread->status = THREADSTATUS_WAIT_MUTEX;
            thread->wakeup_callback = nullptr;

            // Signal that the mutex now has a waiting thread.
            Memory::Write32(thread->mutex_wait_address, mutex_val | Mutex::MutexHasWaitersFlag);

            owner->AddMutexWaiter(thread);

            Core::System::GetInstance().PrepareReschedule();
        }

        ++processed;
    }

    return RESULT_SUCCESS;
}

/// This returns the total CPU ticks elapsed since the CPU was powered-on
static u64 GetSystemTick() {
    const u64 result{CoreTiming::GetTicks()};

    // Advance time to defeat dumb games that busy-wait for the frame to end.
    CoreTiming::AddTicks(400);

    return result;
}

/// Close a handle
static ResultCode CloseHandle(Handle handle) {
    NGLOG_TRACE(Kernel_SVC, "Closing handle {:#010X}", handle);
    return g_handle_table.Close(handle);
}

/// Reset an event
static ResultCode ResetSignal(Handle handle) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called handle {:#010X}", handle);
    auto event = g_handle_table.Get<Event>(handle);
    ASSERT(event != nullptr);
    event->Clear();
    return RESULT_SUCCESS;
}

/// Creates a TransferMemory object
static ResultCode CreateTransferMemory(Handle* handle, VAddr addr, u64 size, u32 permissions) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called addr={:#X}, size={:#X}, perms={:010X}", addr, size,
                  permissions);
    *handle = 0;
    return RESULT_SUCCESS;
}

static ResultCode GetThreadCoreMask(Handle handle, u32* mask, u64* unknown) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called, handle={:010X}", handle);
    *mask = 0x0;
    *unknown = 0xf;
    return RESULT_SUCCESS;
}

static ResultCode SetThreadCoreMask(Handle handle, u32 mask, u64 unknown) {
    NGLOG_WARNING(Kernel_SVC, "(STUBBED) called, handle={:#010X}, mask={:#010X}, unknown={:#X}",
                  handle, mask, unknown);
    return RESULT_SUCCESS;
}

static ResultCode CreateSharedMemory(Handle* handle, u64 size, u32 local_permissions,
                                     u32 remote_permissions) {
    NGLOG_TRACE(Kernel_SVC, "called, size={:#X}, localPerms={:#010X}, remotePerms={:#010X}", size,
                local_permissions, remote_permissions);
    auto sharedMemHandle =
        SharedMemory::Create(g_handle_table.Get<Process>(KernelHandle::CurrentProcess), size,
                             static_cast<MemoryPermission>(local_permissions),
                             static_cast<MemoryPermission>(remote_permissions));

    CASCADE_RESULT(*handle, g_handle_table.Create(sharedMemHandle));
    return RESULT_SUCCESS;
}

static ResultCode ClearEvent(Handle handle) {
    NGLOG_TRACE(Kernel_SVC, "called, event={:010X}", handle);

    SharedPtr<Event> evt = g_handle_table.Get<Event>(handle);
    if (evt == nullptr)
        return ERR_INVALID_HANDLE;
    evt->Clear();
    return RESULT_SUCCESS;
}

namespace {
struct FunctionDef {
    using Func = void();

    u32 id;
    Func* func;
    const char* name;
};
} // namespace

static const FunctionDef SVC_Table[] = {
    {0x00, nullptr, "Unknown"},
    {0x01, SvcWrap<SetHeapSize>, "SetHeapSize"},
    {0x02, nullptr, "SetMemoryPermission"},
    {0x03, SvcWrap<SetMemoryAttribute>, "SetMemoryAttribute"},
    {0x04, SvcWrap<MapMemory>, "MapMemory"},
    {0x05, SvcWrap<UnmapMemory>, "UnmapMemory"},
    {0x06, SvcWrap<QueryMemory>, "QueryMemory"},
    {0x07, SvcWrap<ExitProcess>, "ExitProcess"},
    {0x08, SvcWrap<CreateThread>, "CreateThread"},
    {0x09, SvcWrap<StartThread>, "StartThread"},
    {0x0A, SvcWrap<ExitThread>, "ExitThread"},
    {0x0B, SvcWrap<SleepThread>, "SleepThread"},
    {0x0C, SvcWrap<GetThreadPriority>, "GetThreadPriority"},
    {0x0D, SvcWrap<SetThreadPriority>, "SetThreadPriority"},
    {0x0E, SvcWrap<GetThreadCoreMask>, "GetThreadCoreMask"},
    {0x0F, SvcWrap<SetThreadCoreMask>, "SetThreadCoreMask"},
    {0x10, SvcWrap<GetCurrentProcessorNumber>, "GetCurrentProcessorNumber"},
    {0x11, nullptr, "SignalEvent"},
    {0x12, SvcWrap<ClearEvent>, "ClearEvent"},
    {0x13, SvcWrap<MapSharedMemory>, "MapSharedMemory"},
    {0x14, SvcWrap<UnmapSharedMemory>, "UnmapSharedMemory"},
    {0x15, SvcWrap<CreateTransferMemory>, "CreateTransferMemory"},
    {0x16, SvcWrap<CloseHandle>, "CloseHandle"},
    {0x17, SvcWrap<ResetSignal>, "ResetSignal"},
    {0x18, SvcWrap<WaitSynchronization>, "WaitSynchronization"},
    {0x19, SvcWrap<CancelSynchronization>, "CancelSynchronization"},
    {0x1A, SvcWrap<ArbitrateLock>, "ArbitrateLock"},
    {0x1B, SvcWrap<ArbitrateUnlock>, "ArbitrateUnlock"},
    {0x1C, SvcWrap<WaitProcessWideKeyAtomic>, "WaitProcessWideKeyAtomic"},
    {0x1D, SvcWrap<SignalProcessWideKey>, "SignalProcessWideKey"},
    {0x1E, SvcWrap<GetSystemTick>, "GetSystemTick"},
    {0x1F, SvcWrap<ConnectToNamedPort>, "ConnectToNamedPort"},
    {0x20, nullptr, "SendSyncRequestLight"},
    {0x21, SvcWrap<SendSyncRequest>, "SendSyncRequest"},
    {0x22, nullptr, "SendSyncRequestWithUserBuffer"},
    {0x23, nullptr, "SendAsyncRequestWithUserBuffer"},
    {0x24, SvcWrap<GetProcessId>, "GetProcessId"},
    {0x25, SvcWrap<GetThreadId>, "GetThreadId"},
    {0x26, SvcWrap<Break>, "Break"},
    {0x27, SvcWrap<OutputDebugString>, "OutputDebugString"},
    {0x28, nullptr, "ReturnFromException"},
    {0x29, SvcWrap<GetInfo>, "GetInfo"},
    {0x2A, nullptr, "FlushEntireDataCache"},
    {0x2B, nullptr, "FlushDataCache"},
    {0x2C, nullptr, "MapPhysicalMemory"},
    {0x2D, nullptr, "UnmapPhysicalMemory"},
    {0x2E, nullptr, "GetNextThreadInfo"},
    {0x2F, nullptr, "GetLastThreadInfo"},
    {0x30, nullptr, "GetResourceLimitLimitValue"},
    {0x31, nullptr, "GetResourceLimitCurrentValue"},
    {0x32, SvcWrap<SetThreadActivity>, "SetThreadActivity"},
    {0x33, SvcWrap<GetThreadContext>, "GetThreadContext"},
    {0x34, nullptr, "WaitForAddress"},
    {0x35, nullptr, "SignalToAddress"},
    {0x36, nullptr, "Unknown"},
    {0x37, nullptr, "Unknown"},
    {0x38, nullptr, "Unknown"},
    {0x39, nullptr, "Unknown"},
    {0x3A, nullptr, "Unknown"},
    {0x3B, nullptr, "Unknown"},
    {0x3C, nullptr, "DumpInfo"},
    {0x3D, nullptr, "DumpInfoNew"},
    {0x3E, nullptr, "Unknown"},
    {0x3F, nullptr, "Unknown"},
    {0x40, nullptr, "CreateSession"},
    {0x41, nullptr, "AcceptSession"},
    {0x42, nullptr, "ReplyAndReceiveLight"},
    {0x43, nullptr, "ReplyAndReceive"},
    {0x44, nullptr, "ReplyAndReceiveWithUserBuffer"},
    {0x45, nullptr, "CreateEvent"},
    {0x46, nullptr, "Unknown"},
    {0x47, nullptr, "Unknown"},
    {0x48, nullptr, "AllocateUnsafeMemory"},
    {0x49, nullptr, "FreeUnsafeMemory"},
    {0x4A, nullptr, "SetUnsafeAllocationLimit"},
    {0x4B, nullptr, "CreateJitMemory"},
    {0x4C, nullptr, "MapJitMemory"},
    {0x4D, nullptr, "SleepSystem"},
    {0x4E, nullptr, "ReadWriteRegister"},
    {0x4F, nullptr, "SetProcessActivity"},
    {0x50, SvcWrap<CreateSharedMemory>, "CreateSharedMemory"},
    {0x51, nullptr, "MapTransferMemory"},
    {0x52, nullptr, "UnmapTransferMemory"},
    {0x53, nullptr, "CreateInterruptEvent"},
    {0x54, nullptr, "QueryPhysicalAddress"},
    {0x55, nullptr, "QueryIoMapping"},
    {0x56, nullptr, "CreateDeviceAddressSpace"},
    {0x57, nullptr, "AttachDeviceAddressSpace"},
    {0x58, nullptr, "DetachDeviceAddressSpace"},
    {0x59, nullptr, "MapDeviceAddressSpaceByForce"},
    {0x5A, nullptr, "MapDeviceAddressSpaceAligned"},
    {0x5B, nullptr, "MapDeviceAddressSpace"},
    {0x5C, nullptr, "UnmapDeviceAddressSpace"},
    {0x5D, nullptr, "InvalidateProcessDataCache"},
    {0x5E, nullptr, "StoreProcessDataCache"},
    {0x5F, nullptr, "FlushProcessDataCache"},
    {0x60, nullptr, "DebugActiveProcess"},
    {0x61, nullptr, "BreakDebugProcess"},
    {0x62, nullptr, "TerminateDebugProcess"},
    {0x63, nullptr, "GetDebugEvent"},
    {0x64, nullptr, "ContinueDebugEvent"},
    {0x65, nullptr, "GetProcessList"},
    {0x66, nullptr, "GetThreadList"},
    {0x67, nullptr, "GetDebugThreadContext"},
    {0x68, nullptr, "SetDebugThreadContext"},
    {0x69, nullptr, "QueryDebugProcessMemory"},
    {0x6A, nullptr, "ReadDebugProcessMemory"},
    {0x6B, nullptr, "WriteDebugProcessMemory"},
    {0x6C, nullptr, "SetHardwareBreakPoint"},
    {0x6D, nullptr, "GetDebugThreadParam"},
    {0x6E, nullptr, "Unknown"},
    {0x6F, nullptr, "GetMemoryInfo"},
    {0x70, nullptr, "CreatePort"},
    {0x71, nullptr, "ManageNamedPort"},
    {0x72, nullptr, "ConnectToPort"},
    {0x73, nullptr, "SetProcessMemoryPermission"},
    {0x74, nullptr, "MapProcessMemory"},
    {0x75, nullptr, "UnmapProcessMemory"},
    {0x76, nullptr, "QueryProcessMemory"},
    {0x77, nullptr, "MapProcessCodeMemory"},
    {0x78, nullptr, "UnmapProcessCodeMemory"},
    {0x79, nullptr, "CreateProcess"},
    {0x7A, nullptr, "StartProcess"},
    {0x7B, nullptr, "TerminateProcess"},
    {0x7C, nullptr, "GetProcessInfo"},
    {0x7D, nullptr, "CreateResourceLimit"},
    {0x7E, nullptr, "SetResourceLimitLimitValue"},
    {0x7F, nullptr, "CallSecureMonitor"},
};

static const FunctionDef* GetSVCInfo(u32 func_num) {
    if (func_num >= std::size(SVC_Table)) {
        NGLOG_ERROR(Kernel_SVC, "Unknown svc={:#04X}", func_num);
        return nullptr;
    }
    return &SVC_Table[func_num];
}

MICROPROFILE_DEFINE(Kernel_SVC, "Kernel", "SVC", MP_RGB(70, 200, 70));

void CallSVC(u32 immediate) {
    MICROPROFILE_SCOPE(Kernel_SVC);

    // Lock the global kernel mutex when we enter the kernel HLE.
    std::lock_guard<std::recursive_mutex> lock(HLE::g_hle_lock);

    const FunctionDef* info = GetSVCInfo(immediate);
    if (info) {
        if (info->func) {
            info->func();
        } else {
            NGLOG_CRITICAL(Kernel_SVC, "Unimplemented SVC function {}(..)", info->name);
        }
    } else {
        NGLOG_CRITICAL(Kernel_SVC, "Unknown SVC function {:#X}", immediate);
    }
}

} // namespace Kernel
