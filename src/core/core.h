// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <string>
#include <thread>
#include "common/common_types.h"
#include "core/arm/exclusive_monitor.h"
#include "core/core_cpu.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/scheduler.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "core/telemetry_session.h"
#include "file_sys/vfs_real.h"
#include "hle/service/filesystem/filesystem.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"

class ARM_Interface;

namespace Core::Frontend {
class EmuWindow;
}

namespace Service::SM {
class ServiceManager;
}

namespace VideoCore {
class RendererBase;
}

namespace Core {

class System {
public:
    ~System();

    /**
     * Gets the instance of the System singleton class.
     * @returns Reference to the instance of the System singleton class.
     */
    static System& GetInstance() {
        return s_instance;
    }

    /// Enumeration representing the return values of the System Initialize and Load process.
    enum class ResultStatus : u32 {
        Success,             ///< Succeeded
        ErrorNotInitialized, ///< Error trying to use core prior to initialization
        ErrorGetLoader,      ///< Error finding the correct application loader
        ErrorSystemMode,     ///< Error determining the system mode
        ErrorSystemFiles,    ///< Error in finding system files
        ErrorSharedFont,     ///< Error in finding shared font
        ErrorVideoCore,      ///< Error in the video core
        ErrorUnknown,        ///< Any other error
        ErrorLoader,         ///< The base for loader errors (too many to repeat)
    };

    /**
     * Run the core CPU loop
     * This function runs the core for the specified number of CPU instructions before trying to
     * update hardware. This is much faster than SingleStep (and should be equivalent), as the CPU
     * is not required to do a full dispatch with each instruction. NOTE: the number of instructions
     * requested is not guaranteed to run, as this will be interrupted preemptively if a hardware
     * update is requested (e.g. on a thread switch).
     * @param tight_loop If false, the CPU single-steps.
     * @return Result status, indicating whether or not the operation succeeded.
     */
    ResultStatus RunLoop(bool tight_loop = true);

    /**
     * Step the CPU one instruction
     * @return Result status, indicating whether or not the operation succeeded.
     */
    ResultStatus SingleStep();

    /**
     * Invalidate the CPU instruction caches
     * This function should only be used by GDB Stub to support breakpoints, memory updates and
     * step/continue commands.
     */
    void InvalidateCpuInstructionCaches() {
        for (auto& cpu : cpu_cores) {
            cpu->ArmInterface().ClearInstructionCache();
        }
    }

    /// Shutdown the emulated system.
    void Shutdown();

    /**
     * Load an executable application.
     * @param emu_window Reference to the host-system window used for video output and keyboard
     *                   input.
     * @param filepath String path to the executable application to load on the host file system.
     * @returns ResultStatus code, indicating if the operation succeeded.
     */
    ResultStatus Load(Frontend::EmuWindow& emu_window, const std::string& filepath);

    /**
     * Indicates if the emulated system is powered on (all subsystems initialized and able to run an
     * application).
     * @returns True if the emulated system is powered on, otherwise false.
     */
    bool IsPoweredOn() const {
        return cpu_barrier && cpu_barrier->IsAlive();
    }

    /**
     * Returns a reference to the telemetry session for this emulation session.
     * @returns Reference to the telemetry session.
     */
    Core::TelemetrySession& TelemetrySession() const {
        return *telemetry_session;
    }

    /// Prepare the core emulation for a reschedule
    void PrepareReschedule();

    /// Gets and resets core performance statistics
    PerfStats::Results GetAndResetPerfStats();

    /// Gets an ARM interface to the CPU core that is currently running
    ARM_Interface& CurrentArmInterface() {
        return CurrentCpuCore().ArmInterface();
    }

    /// Gets the index of the currently running CPU core
    size_t CurrentCoreIndex() {
        return CurrentCpuCore().CoreIndex();
    }

    /// Gets an ARM interface to the CPU core with the specified index
    ARM_Interface& ArmInterface(size_t core_index);

    /// Gets a CPU interface to the CPU core with the specified index
    Cpu& CpuCore(size_t core_index);

    /// Gets a mutable reference to the GPU interface
    Tegra::GPU& GPU() {
        return *gpu_core;
    }

    /// Gets an immutable reference to the GPU interface.
    const Tegra::GPU& GPU() const {
        return *gpu_core;
    }

    /// Gets a mutable reference to the renderer.
    VideoCore::RendererBase& Renderer() {
        return *renderer;
    }

    /// Gets an immutable reference to the renderer.
    const VideoCore::RendererBase& Renderer() const {
        return *renderer;
    }

    /// Gets the scheduler for the CPU core that is currently running
    Kernel::Scheduler& CurrentScheduler() {
        return *CurrentCpuCore().Scheduler();
    }

    /// Gets the exclusive monitor
    ExclusiveMonitor& Monitor() {
        return *cpu_exclusive_monitor;
    }

    /// Gets the scheduler for the CPU core with the specified index
    const std::shared_ptr<Kernel::Scheduler>& Scheduler(size_t core_index);

    /// Gets the current process
    Kernel::SharedPtr<Kernel::Process>& CurrentProcess() {
        return current_process;
    }

    PerfStats perf_stats;
    FrameLimiter frame_limiter;

    void SetStatus(ResultStatus new_status, const char* details = nullptr) {
        status = new_status;
        if (details) {
            status_details = details;
        }
    }

    const std::string& GetStatusDetails() const {
        return status_details;
    }

    Loader::AppLoader& GetAppLoader() const {
        return *app_loader;
    }

    Service::SM::ServiceManager& ServiceManager();
    const Service::SM::ServiceManager& ServiceManager() const;

    void SetGPUDebugContext(std::shared_ptr<Tegra::DebugContext> context) {
        debug_context = std::move(context);
    }

    std::shared_ptr<Tegra::DebugContext> GetGPUDebugContext() const {
        return debug_context;
    }

    void SetFilesystem(FileSys::VirtualFilesystem vfs) {
        virtual_filesystem = std::move(vfs);
    }

    FileSys::VirtualFilesystem GetFilesystem() const {
        return virtual_filesystem;
    }

private:
    System();

    /// Returns the currently running CPU core
    Cpu& CurrentCpuCore();

    /**
     * Initialize the emulated system.
     * @param emu_window Reference to the host-system window used for video output and keyboard
     *                   input.
     * @return ResultStatus code, indicating if the operation succeeded.
     */
    ResultStatus Init(Frontend::EmuWindow& emu_window);

    /// RealVfsFilesystem instance
    FileSys::VirtualFilesystem virtual_filesystem;
    /// AppLoader used to load the current executing application
    std::unique_ptr<Loader::AppLoader> app_loader;
    std::unique_ptr<VideoCore::RendererBase> renderer;
    std::unique_ptr<Tegra::GPU> gpu_core;
    std::shared_ptr<Tegra::DebugContext> debug_context;
    Kernel::SharedPtr<Kernel::Process> current_process;
    std::shared_ptr<ExclusiveMonitor> cpu_exclusive_monitor;
    std::shared_ptr<CpuBarrier> cpu_barrier;
    std::array<std::shared_ptr<Cpu>, NUM_CPU_CORES> cpu_cores;
    std::array<std::unique_ptr<std::thread>, NUM_CPU_CORES - 1> cpu_core_threads;
    size_t active_core{}; ///< Active core, only used in single thread mode

    /// Service manager
    std::shared_ptr<Service::SM::ServiceManager> service_manager;

    /// Telemetry session for this emulation session
    std::unique_ptr<Core::TelemetrySession> telemetry_session;

    static System s_instance;

    ResultStatus status = ResultStatus::Success;
    std::string status_details = "";

    /// Map of guest threads to CPU cores
    std::map<std::thread::id, std::shared_ptr<Cpu>> thread_to_cpu;
};

inline ARM_Interface& CurrentArmInterface() {
    return System::GetInstance().CurrentArmInterface();
}

inline TelemetrySession& Telemetry() {
    return System::GetInstance().TelemetrySession();
}

inline Kernel::SharedPtr<Kernel::Process>& CurrentProcess() {
    return System::GetInstance().CurrentProcess();
}

} // namespace Core
