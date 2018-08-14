// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <iterator>
#include <mutex>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/mutex.h"
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
    LOG_TRACE(Kernel_SVC, "called, heap_size=0x{:X}", heap_size);
    auto& process = *Core::CurrentProcess();
    CASCADE_RESULT(*heap_addr,
                   process.HeapAllocate(Memory::HEAP_VADDR, heap_size, VMAPermission::ReadWrite));
    return RESULT_SUCCESS;
}

static ResultCode SetMemoryAttribute(VAddr addr, u64 size, u32 state0, u32 state1) {
    LOG_WARNING(Kernel_SVC,
                "(STUBBED) called, addr=0x{:X}, size=0x{:X}, state0=0x{:X}, state1=0x{:X}", addr,
                size, state0, state1);
    return RESULT_SUCCESS;
}

/// Maps a memory range into a different range.
static ResultCode MapMemory(VAddr dst_addr, VAddr src_addr, u64 size) {
    LOG_TRACE(Kernel_SVC, "called, dst_addr=0x{:X}, src_addr=0x{:X}, size=0x{:X}", dst_addr,
              src_addr, size);
    return Core::CurrentProcess()->MirrorMemory(dst_addr, src_addr, size);
}

/// Unmaps a region that was previously mapped with svcMapMemory
static ResultCode UnmapMemory(VAddr dst_addr, VAddr src_addr, u64 size) {
    LOG_TRACE(Kernel_SVC, "called, dst_addr=0x{:X}, src_addr=0x{:X}, size=0x{:X}", dst_addr,
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

    LOG_TRACE(Kernel_SVC, "called port_name={}", port_name);

    auto it = Service::g_kernel_named_ports.find(port_name);
    if (it == Service::g_kernel_named_ports.end()) {
        LOG_WARNING(Kernel_SVC, "tried to connect to unknown port: {}", port_name);
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
        LOG_ERROR(Kernel_SVC, "called with invalid handle=0x{:08X}", handle);
        return ERR_INVALID_HANDLE;
    }

    LOG_TRACE(Kernel_SVC, "called handle=0x{:08X}({})", handle, session->GetName());

    Core::System::GetInstance().PrepareReschedule();

    // TODO(Subv): svcSendSyncRequest should put the caller thread to sleep while the server
    // responds and cause a reschedule.
    return session->SendSyncRequest(GetCurrentThread());
}

/// Get the ID for the specified thread.
static ResultCode GetThreadId(u32* thread_id, Handle thread_handle) {
    LOG_TRACE(Kernel_SVC, "called thread=0x{:08X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    *thread_id = thread->GetThreadId();
    return RESULT_SUCCESS;
}

/// Get the ID of the specified process
static ResultCode GetProcessId(u32* process_id, Handle process_handle) {
    LOG_TRACE(Kernel_SVC, "called process=0x{:08X}", process_handle);

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
    ASSERT(thread->status == ThreadStatus::WaitSynchAny);

    if (reason == ThreadWakeupReason::Timeout) {
        thread->SetWaitSynchronizationResult(RESULT_TIMEOUT);
        return true;
    }

    ASSERT(reason == ThreadWakeupReason::Signal);
    thread->SetWaitSynchronizationResult(RESULT_SUCCESS);
    thread->SetWaitSynchronizationOutput(static_cast<u32>(index));
    return true;
};

/// Wait for the given handles to synchronize, timeout after the specified nanoseconds
static ResultCode WaitSynchronization(Handle* index, VAddr handles_address, u64 handle_count,
                                      s64 nano_seconds) {
    LOG_TRACE(Kernel_SVC, "called handles_address=0x{:X}, handle_count={}, nano_seconds={}",
              handles_address, handle_count, nano_seconds);

    if (!Memory::IsValidVirtualAddress(handles_address))
        return ERR_INVALID_POINTER;

    static constexpr u64 MaxHandles = 0x40;

    if (handle_count > MaxHandles)
        return ResultCode(ErrorModule::Kernel, ErrCodes::TooLarge);

    auto thread = GetCurrentThread();

    using ObjectPtr = SharedPtr<WaitObject>;
    std::vector<ObjectPtr> objects(handle_count);

    for (u64 i = 0; i < handle_count; ++i) {
        const Handle handle = Memory::Read32(handles_address + i * sizeof(Handle));
        const auto object = g_handle_table.Get<WaitObject>(handle);

        if (object == nullptr) {
            return ERR_INVALID_HANDLE;
        }

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
    thread->status = ThreadStatus::WaitSynchAny;

    // Create an event to wake the thread up after the specified nanosecond delay has passed
    thread->WakeAfterDelay(nano_seconds);
    thread->wakeup_callback = DefaultThreadWakeupCallback;

    Core::System::GetInstance().CpuCore(thread->processor_id).PrepareReschedule();

    return RESULT_TIMEOUT;
}

/// Resumes a thread waiting on WaitSynchronization
static ResultCode CancelSynchronization(Handle thread_handle) {
    LOG_TRACE(Kernel_SVC, "called thread=0x{:X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    ASSERT(thread->status == ThreadStatus::WaitSynchAny);
    thread->SetWaitSynchronizationResult(
        ResultCode(ErrorModule::Kernel, ErrCodes::SynchronizationCanceled));
    thread->ResumeFromWait();
    return RESULT_SUCCESS;
}

/// Attempts to locks a mutex, creating it if it does not already exist
static ResultCode ArbitrateLock(Handle holding_thread_handle, VAddr mutex_addr,
                                Handle requesting_thread_handle) {
    LOG_TRACE(Kernel_SVC,
              "called holding_thread_handle=0x{:08X}, mutex_addr=0x{:X}, "
              "requesting_current_thread_handle=0x{:08X}",
              holding_thread_handle, mutex_addr, requesting_thread_handle);

    return Mutex::TryAcquire(mutex_addr, holding_thread_handle, requesting_thread_handle);
}

/// Unlock a mutex
static ResultCode ArbitrateUnlock(VAddr mutex_addr) {
    LOG_TRACE(Kernel_SVC, "called mutex_addr=0x{:X}", mutex_addr);

    return Mutex::Release(mutex_addr);
}

/// Break program execution
static void Break(u64 unk_0, u64 unk_1, u64 unk_2) {
    LOG_CRITICAL(Debug_Emulated, "Emulated program broke execution!");
    ASSERT(false);
}

/// Used to output a message on a debug hardware unit - does nothing on a retail unit
static void OutputDebugString(VAddr address, s32 len) {
    std::string str(len, '\0');
    Memory::ReadBlock(address, str.data(), str.size());
    LOG_DEBUG(Debug_Emulated, "{}", str);
}

/// Gets system/memory information for the current process
static ResultCode GetInfo(u64* result, u64 info_id, u64 handle, u64 info_sub_id) {
    LOG_TRACE(Kernel_SVC, "called info_id=0x{:X}, info_sub_id=0x{:X}, handle=0x{:08X}", info_id,
              info_sub_id, handle);

    const auto& vm_manager = Core::CurrentProcess()->vm_manager;

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
        LOG_WARNING(Kernel_SVC, "(STUBBED) Attempted to query titleid, returned 0");
        *result = 0;
        break;
    case GetInfoType::PrivilegedProcessId:
        LOG_WARNING(Kernel_SVC,
                    "(STUBBED) Attempted to query privileged process id bounds, returned 0");
        *result = 0;
        break;
    case GetInfoType::UserExceptionContextAddr:
        LOG_WARNING(Kernel_SVC,
                    "(STUBBED) Attempted to query user exception context address, returned 0");
        *result = 0;
        break;
    default:
        UNIMPLEMENTED();
    }

    return RESULT_SUCCESS;
}

/// Sets the thread activity
static ResultCode SetThreadActivity(Handle handle, u32 unknown) {
    LOG_WARNING(Kernel_SVC, "(STUBBED) called, handle=0x{:08X}, unknown=0x{:08X}", handle, unknown);
    return RESULT_SUCCESS;
}

/// Gets the thread context
static ResultCode GetThreadContext(Handle handle, VAddr addr) {
    LOG_WARNING(Kernel_SVC, "(STUBBED) called, handle=0x{:08X}, addr=0x{:X}", handle, addr);
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

    Core::System::GetInstance().CpuCore(thread->processor_id).PrepareReschedule();
    return RESULT_SUCCESS;
}

/// Get which CPU core is executing the current thread
static u32 GetCurrentProcessorNumber() {
    LOG_TRACE(Kernel_SVC, "called");
    return GetCurrentThread()->processor_id;
}

static ResultCode MapSharedMemory(Handle shared_memory_handle, VAddr addr, u64 size,
                                  u32 permissions) {
    LOG_TRACE(Kernel_SVC,
              "called, shared_memory_handle=0x{:X}, addr=0x{:X}, size=0x{:X}, permissions=0x{:08X}",
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
        LOG_ERROR(Kernel_SVC, "unknown permissions=0x{:08X}", permissions);
    }

    return RESULT_SUCCESS;
}

static ResultCode UnmapSharedMemory(Handle shared_memory_handle, VAddr addr, u64 size) {
    LOG_WARNING(Kernel_SVC, "called, shared_memory_handle=0x{:08X}, addr=0x{:X}, size=0x{:X}",
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

    LOG_TRACE(Kernel_SVC, "called process=0x{:08X} addr={:X}", process_handle, addr);
    return RESULT_SUCCESS;
}

/// Query memory
static ResultCode QueryMemory(MemoryInfo* memory_info, PageInfo* page_info, VAddr addr) {
    LOG_TRACE(Kernel_SVC, "called, addr={:X}", addr);
    return QueryProcessMemory(memory_info, page_info, CurrentProcess, addr);
}

/// Exits the current process
static void ExitProcess() {
    LOG_INFO(Kernel_SVC, "Process {} exiting", Core::CurrentProcess()->process_id);

    ASSERT_MSG(Core::CurrentProcess()->status == ProcessStatus::Running,
               "Process has already exited");

    Core::CurrentProcess()->status = ProcessStatus::Exited;

    auto stop_threads = [](const std::vector<SharedPtr<Thread>>& thread_list) {
        for (auto& thread : thread_list) {
            if (thread->owner_process != Core::CurrentProcess())
                continue;

            if (thread == GetCurrentThread())
                continue;

            // TODO(Subv): When are the other running/ready threads terminated?
            ASSERT_MSG(thread->status == ThreadStatus::WaitSynchAny ||
                           thread->status == ThreadStatus::WaitSynchAll,
                       "Exiting processes with non-waiting threads is currently unimplemented");

            thread->Stop();
        }
    };

    auto& system = Core::System::GetInstance();
    stop_threads(system.Scheduler(0)->GetThreadList());
    stop_threads(system.Scheduler(1)->GetThreadList());
    stop_threads(system.Scheduler(2)->GetThreadList());
    stop_threads(system.Scheduler(3)->GetThreadList());

    // Kill the current thread
    GetCurrentThread()->Stop();

    Core::System::GetInstance().PrepareReschedule();
}

/// Creates a new thread
static ResultCode CreateThread(Handle* out_handle, VAddr entry_point, u64 arg, VAddr stack_top,
                               u32 priority, s32 processor_id) {
    std::string name = fmt::format("unknown-{:X}", entry_point);

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
    case THREADPROCESSORID_1:
    case THREADPROCESSORID_2:
    case THREADPROCESSORID_3:
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

    Core::System::GetInstance().CpuCore(thread->processor_id).PrepareReschedule();

    LOG_TRACE(Kernel_SVC,
              "called entrypoint=0x{:08X} ({}), arg=0x{:08X}, stacktop=0x{:08X}, "
              "threadpriority=0x{:08X}, processorid=0x{:08X} : created handle=0x{:08X}",
              entry_point, name, arg, stack_top, priority, processor_id, *out_handle);

    return RESULT_SUCCESS;
}

/// Starts the thread for the provided handle
static ResultCode StartThread(Handle thread_handle) {
    LOG_TRACE(Kernel_SVC, "called thread=0x{:08X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    ASSERT(thread->status == ThreadStatus::Dormant);

    thread->ResumeFromWait();
    Core::System::GetInstance().CpuCore(thread->processor_id).PrepareReschedule();

    return RESULT_SUCCESS;
}

/// Called when a thread exits
static void ExitThread() {
    LOG_TRACE(Kernel_SVC, "called, pc=0x{:08X}", Core::CurrentArmInterface().GetPC());

    ExitCurrentThread();
    Core::System::GetInstance().PrepareReschedule();
}

/// Sleep the current thread
static void SleepThread(s64 nanoseconds) {
    LOG_TRACE(Kernel_SVC, "called nanoseconds={}", nanoseconds);

    // Don't attempt to yield execution if there are no available threads to run,
    // this way we avoid a useless reschedule to the idle thread.
    if (nanoseconds == 0 && !Core::System::GetInstance().CurrentScheduler().HaveReadyThreads())
        return;

    // Sleep current thread and check for next thread to schedule
    WaitCurrentThread_Sleep();

    // Create an event to wake the thread up after the specified nanosecond delay has passed
    GetCurrentThread()->WakeAfterDelay(nanoseconds);

    Core::System::GetInstance().PrepareReschedule();
}

/// Wait process wide key atomic
static ResultCode WaitProcessWideKeyAtomic(VAddr mutex_addr, VAddr condition_variable_addr,
                                           Handle thread_handle, s64 nano_seconds) {
    LOG_TRACE(
        Kernel_SVC,
        "called mutex_addr={:X}, condition_variable_addr={:X}, thread_handle=0x{:08X}, timeout={}",
        mutex_addr, condition_variable_addr, thread_handle, nano_seconds);

    SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    ASSERT(thread);

    CASCADE_CODE(Mutex::Release(mutex_addr));

    SharedPtr<Thread> current_thread = GetCurrentThread();
    current_thread->condvar_wait_address = condition_variable_addr;
    current_thread->mutex_wait_address = mutex_addr;
    current_thread->wait_handle = thread_handle;
    current_thread->status = ThreadStatus::WaitMutex;
    current_thread->wakeup_callback = nullptr;

    current_thread->WakeAfterDelay(nano_seconds);

    // Note: Deliberately don't attempt to inherit the lock owner's priority.

    Core::System::GetInstance().CpuCore(current_thread->processor_id).PrepareReschedule();
    return RESULT_SUCCESS;
}

/// Signal process wide key
static ResultCode SignalProcessWideKey(VAddr condition_variable_addr, s32 target) {
    LOG_TRACE(Kernel_SVC, "called, condition_variable_addr=0x{:X}, target=0x{:08X}",
              condition_variable_addr, target);

    auto RetrieveWaitingThreads =
        [](size_t core_index, std::vector<SharedPtr<Thread>>& waiting_threads, VAddr condvar_addr) {
            const auto& scheduler = Core::System::GetInstance().Scheduler(core_index);
            auto& thread_list = scheduler->GetThreadList();

            for (auto& thread : thread_list) {
                if (thread->condvar_wait_address == condvar_addr)
                    waiting_threads.push_back(thread);
            }
        };

    // Retrieve a list of all threads that are waiting for this condition variable.
    std::vector<SharedPtr<Thread>> waiting_threads;
    RetrieveWaitingThreads(0, waiting_threads, condition_variable_addr);
    RetrieveWaitingThreads(1, waiting_threads, condition_variable_addr);
    RetrieveWaitingThreads(2, waiting_threads, condition_variable_addr);
    RetrieveWaitingThreads(3, waiting_threads, condition_variable_addr);
    // Sort them by priority, such that the highest priority ones come first.
    std::sort(waiting_threads.begin(), waiting_threads.end(),
              [](const SharedPtr<Thread>& lhs, const SharedPtr<Thread>& rhs) {
                  return lhs->current_priority < rhs->current_priority;
              });

    // Only process up to 'target' threads, unless 'target' is -1, in which case process
    // them all.
    size_t last = waiting_threads.size();
    if (target != -1)
        last = target;

    // If there are no threads waiting on this condition variable, just exit
    if (last > waiting_threads.size())
        return RESULT_SUCCESS;

    for (size_t index = 0; index < last; ++index) {
        auto& thread = waiting_threads[index];

        ASSERT(thread->condvar_wait_address == condition_variable_addr);

        size_t current_core = Core::System::GetInstance().CurrentCoreIndex();

        auto& monitor = Core::System::GetInstance().Monitor();

        // Atomically read the value of the mutex.
        u32 mutex_val = 0;
        do {
            monitor.SetExclusive(current_core, thread->mutex_wait_address);

            // If the mutex is not yet acquired, acquire it.
            mutex_val = Memory::Read32(thread->mutex_wait_address);

            if (mutex_val != 0) {
                monitor.ClearExclusive();
                break;
            }
        } while (!monitor.ExclusiveWrite32(current_core, thread->mutex_wait_address,
                                           thread->wait_handle));

        if (mutex_val == 0) {
            // We were able to acquire the mutex, resume this thread.
            ASSERT(thread->status == ThreadStatus::WaitMutex);
            thread->ResumeFromWait();

            auto lock_owner = thread->lock_owner;
            if (lock_owner)
                lock_owner->RemoveMutexWaiter(thread);

            thread->lock_owner = nullptr;
            thread->mutex_wait_address = 0;
            thread->condvar_wait_address = 0;
            thread->wait_handle = 0;
        } else {
            // Atomically signal that the mutex now has a waiting thread.
            do {
                monitor.SetExclusive(current_core, thread->mutex_wait_address);

                // Ensure that the mutex value is still what we expect.
                u32 value = Memory::Read32(thread->mutex_wait_address);
                // TODO(Subv): When this happens, the kernel just clears the exclusive state and
                // retries the initial read for this thread.
                ASSERT_MSG(mutex_val == value, "Unhandled synchronization primitive case");
            } while (!monitor.ExclusiveWrite32(current_core, thread->mutex_wait_address,
                                               mutex_val | Mutex::MutexHasWaitersFlag));

            // The mutex is already owned by some other thread, make this thread wait on it.
            Handle owner_handle = static_cast<Handle>(mutex_val & Mutex::MutexOwnerMask);
            auto owner = g_handle_table.Get<Thread>(owner_handle);
            ASSERT(owner);
            ASSERT(thread->status == ThreadStatus::WaitMutex);
            thread->wakeup_callback = nullptr;

            owner->AddMutexWaiter(thread);

            Core::System::GetInstance().CpuCore(thread->processor_id).PrepareReschedule();
        }
    }

    return RESULT_SUCCESS;
}

// Wait for an address (via Address Arbiter)
static ResultCode WaitForAddress(VAddr address, u32 type, s32 value, s64 timeout) {
    LOG_WARNING(Kernel_SVC, "called, address=0x{:X}, type=0x{:X}, value=0x{:X}, timeout={}",
                address, type, value, timeout);
    // If the passed address is a kernel virtual address, return invalid memory state.
    if (Memory::IsKernelVirtualAddress(address)) {
        return ERR_INVALID_ADDRESS_STATE;
    }
    // If the address is not properly aligned to 4 bytes, return invalid address.
    if (address % sizeof(u32) != 0) {
        return ERR_INVALID_ADDRESS;
    }

    switch (static_cast<AddressArbiter::ArbitrationType>(type)) {
    case AddressArbiter::ArbitrationType::WaitIfLessThan:
        return AddressArbiter::WaitForAddressIfLessThan(address, value, timeout, false);
    case AddressArbiter::ArbitrationType::DecrementAndWaitIfLessThan:
        return AddressArbiter::WaitForAddressIfLessThan(address, value, timeout, true);
    case AddressArbiter::ArbitrationType::WaitIfEqual:
        return AddressArbiter::WaitForAddressIfEqual(address, value, timeout);
    default:
        return ERR_INVALID_ENUM_VALUE;
    }
}

// Signals to an address (via Address Arbiter)
static ResultCode SignalToAddress(VAddr address, u32 type, s32 value, s32 num_to_wake) {
    LOG_WARNING(Kernel_SVC, "called, address=0x{:X}, type=0x{:X}, value=0x{:X}, num_to_wake=0x{:X}",
                address, type, value, num_to_wake);
    // If the passed address is a kernel virtual address, return invalid memory state.
    if (Memory::IsKernelVirtualAddress(address)) {
        return ERR_INVALID_ADDRESS_STATE;
    }
    // If the address is not properly aligned to 4 bytes, return invalid address.
    if (address % sizeof(u32) != 0) {
        return ERR_INVALID_ADDRESS;
    }

    switch (static_cast<AddressArbiter::SignalType>(type)) {
    case AddressArbiter::SignalType::Signal:
        return AddressArbiter::SignalToAddress(address, num_to_wake);
    case AddressArbiter::SignalType::IncrementAndSignalIfEqual:
        return AddressArbiter::IncrementAndSignalToAddressIfEqual(address, value, num_to_wake);
    case AddressArbiter::SignalType::ModifyByWaitingCountAndSignalIfEqual:
        return AddressArbiter::ModifyByWaitingCountAndSignalToAddressIfEqual(address, value,
                                                                             num_to_wake);
    default:
        return ERR_INVALID_ENUM_VALUE;
    }
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
    LOG_TRACE(Kernel_SVC, "Closing handle 0x{:08X}", handle);
    return g_handle_table.Close(handle);
}

/// Reset an event
static ResultCode ResetSignal(Handle handle) {
    LOG_WARNING(Kernel_SVC, "(STUBBED) called handle 0x{:08X}", handle);
    auto event = g_handle_table.Get<Event>(handle);
    ASSERT(event != nullptr);
    event->Clear();
    return RESULT_SUCCESS;
}

/// Creates a TransferMemory object
static ResultCode CreateTransferMemory(Handle* handle, VAddr addr, u64 size, u32 permissions) {
    LOG_WARNING(Kernel_SVC, "(STUBBED) called addr=0x{:X}, size=0x{:X}, perms=0x{:08X}", addr, size,
                permissions);
    *handle = 0;
    return RESULT_SUCCESS;
}

static ResultCode GetThreadCoreMask(Handle thread_handle, u32* core, u64* mask) {
    LOG_TRACE(Kernel_SVC, "called, handle=0x{:08X}", thread_handle);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    *core = thread->ideal_core;
    *mask = thread->affinity_mask;

    return RESULT_SUCCESS;
}

static ResultCode SetThreadCoreMask(Handle thread_handle, u32 core, u64 mask) {
    LOG_DEBUG(Kernel_SVC, "called, handle=0x{:08X}, mask=0x{:16X}, core=0x{:X}", thread_handle,
              mask, core);

    const SharedPtr<Thread> thread = g_handle_table.Get<Thread>(thread_handle);
    if (!thread) {
        return ERR_INVALID_HANDLE;
    }

    if (core == static_cast<u32>(THREADPROCESSORID_DEFAULT)) {
        ASSERT(thread->owner_process->ideal_processor !=
               static_cast<u8>(THREADPROCESSORID_DEFAULT));
        // Set the target CPU to the one specified in the process' exheader.
        core = thread->owner_process->ideal_processor;
        mask = 1ull << core;
    }

    if (mask == 0) {
        return ResultCode(ErrorModule::Kernel, ErrCodes::InvalidCombination);
    }

    /// This value is used to only change the affinity mask without changing the current ideal core.
    static constexpr u32 OnlyChangeMask = static_cast<u32>(-3);

    if (core == OnlyChangeMask) {
        core = thread->ideal_core;
    } else if (core >= Core::NUM_CPU_CORES && core != static_cast<u32>(-1)) {
        return ResultCode(ErrorModule::Kernel, ErrCodes::InvalidProcessorId);
    }

    // Error out if the input core isn't enabled in the input mask.
    if (core < Core::NUM_CPU_CORES && (mask & (1ull << core)) == 0) {
        return ResultCode(ErrorModule::Kernel, ErrCodes::InvalidCombination);
    }

    thread->ChangeCore(core, mask);

    return RESULT_SUCCESS;
}

static ResultCode CreateSharedMemory(Handle* handle, u64 size, u32 local_permissions,
                                     u32 remote_permissions) {
    LOG_TRACE(Kernel_SVC, "called, size=0x{:X}, localPerms=0x{:08X}, remotePerms=0x{:08X}", size,
              local_permissions, remote_permissions);
    auto sharedMemHandle =
        SharedMemory::Create(g_handle_table.Get<Process>(KernelHandle::CurrentProcess), size,
                             static_cast<MemoryPermission>(local_permissions),
                             static_cast<MemoryPermission>(remote_permissions));

    CASCADE_RESULT(*handle, g_handle_table.Create(sharedMemHandle));
    return RESULT_SUCCESS;
}

static ResultCode ClearEvent(Handle handle) {
    LOG_TRACE(Kernel_SVC, "called, event=0x{:08X}", handle);

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
    {0x34, SvcWrap<WaitForAddress>, "WaitForAddress"},
    {0x35, SvcWrap<SignalToAddress>, "SignalToAddress"},
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
        LOG_ERROR(Kernel_SVC, "Unknown svc=0x{:02X}", func_num);
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
            LOG_CRITICAL(Kernel_SVC, "Unimplemented SVC function {}(..)", info->name);
        }
    } else {
        LOG_CRITICAL(Kernel_SVC, "Unknown SVC function 0x{:X}", immediate);
    }
}

} // namespace Kernel
