#include "os.h"
#include "loader/nro.h"
#include "nce/guest.h"

namespace skyline::kernel {
    OS::OS(std::shared_ptr<JvmManager> &jvmManager, std::shared_ptr<Logger> &logger, std::shared_ptr<Settings> &settings) : state(this, process, jvmManager, settings, logger), serviceManager(state) {}

    void OS::Execute(const int romFd, const TitleFormat romType) {
        std::shared_ptr<loader::Loader> loader;
        if (romType == TitleFormat::NRO) {
            loader = std::make_shared<loader::NroLoader>(romFd);
        } else
            throw exception("Unsupported ROM extension.");
        auto process = CreateProcess(loader->mainEntry, 0, constant::DefStackSize);
        loader->LoadProcessData(process, state);
        process->threadMap.at(process->pid)->Start(); // The kernel itself is responsible for starting the main thread
        state.nce->Execute();
    }

    std::shared_ptr<type::KProcess> OS::CreateProcess(u64 entry, u64 argument, size_t stackSize) {
        madvise(reinterpret_cast<void *>(constant::BaseAddr), constant::BaseEnd, MADV_DONTFORK);
        auto *stack = static_cast<u8 *>(mmap(nullptr, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS | MAP_STACK, -1, 0)); // NOLINT(hicpp-signed-bitwise)
        madvise(stack, reinterpret_cast<size_t>(stack) + stackSize, MADV_DOFORK);
        if (stack == MAP_FAILED)
            throw exception("Failed to allocate stack memory");
        if (mprotect(stack, PAGE_SIZE, PROT_NONE)) {
            munmap(stack, stackSize);
            throw exception("Failed to create guard pages");
        }
        auto tlsMem = std::make_shared<type::KSharedMemory>(state, 0, (sizeof(ThreadContext) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1), memory::Permission(true, true, 0), memory::Type::Reserved); // NOLINT(hicpp-signed-bitwise)
        tlsMem->guest = tlsMem->kernel;
        madvise(reinterpret_cast<void *>(tlsMem->guest.address), tlsMem->guest.size, MADV_DOFORK);
        pid_t pid = clone(reinterpret_cast<int (*)(void *)>(&guest::entry), stack + stackSize, CLONE_FILES | CLONE_FS | CLONE_SETTLS | SIGCHLD, reinterpret_cast<void *>(entry), nullptr, reinterpret_cast<void*>(tlsMem->guest.address)); // NOLINT(hicpp-signed-bitwise)
        if (pid == -1)
            throw exception("Call to clone() has failed: {}", strerror(errno));
        process = std::make_shared<kernel::type::KProcess>(state, pid, argument, reinterpret_cast<u64>(stack), stackSize, tlsMem);
        state.logger->Debug("Successfully created process with PID: {}", pid);
        return process;
    }

    void OS::KillThread(pid_t pid) {
        if (process->pid == pid) {
            state.logger->Debug("Killing process with PID: {}", pid);
            for (auto&[key, value]: process->threadMap) {
                value->Kill();
            }
            process.reset();
        } else {
            state.logger->Debug("Killing thread with TID: {}", pid);
            process->threadMap.at(pid)->Kill();
            process->threadMap.erase(pid);
        }
    }
}
