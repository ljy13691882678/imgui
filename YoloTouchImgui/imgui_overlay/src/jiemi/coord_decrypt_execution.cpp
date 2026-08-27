#include "jiemi/coord_decrypt_internal.h"

#include "jiemi/qarma_pac.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(JIEMI_WITH_UNICORN)
#include <unicorn/unicorn.h>
#endif

namespace CoordDecrypt::detail
{
namespace
{
constexpr uint64_t kExitAddress = 0x0000007FFF000000ULL;
constexpr uint64_t kStackBase = 0x0000007FFE000000ULL;
constexpr uint64_t kStackSize = 0x00100000ULL;
constexpr uint64_t kInstructionLimit = 100000ULL;
constexpr uint64_t kEmulationTimeoutMicroseconds = 500000ULL;
constexpr uint32_t kRemotePageLimit = 4096;
constexpr uint32_t kInstructionRecoveryLimit = 8;
constexpr uint64_t kMaximumLinuxFileDescriptor =
    static_cast<uint64_t>(std::numeric_limits<int>::max());
constexpr uint64_t kShellcodeIoctlCommand = 0xC030094CULL;
constexpr uint64_t kShellcodeIoctlBufferSize = 0x30ULL;
constexpr int64_t kShellcodeIoctlResult = -22;
constexpr uint64_t kMaximumRingSlot = 0x1000ULL;
constexpr uint32_t kSvcInstruction = 0xD4000001U;
constexpr uint32_t kSnapshotReplayRounds = 3;
constexpr uint32_t kPlannedSnapshotReplayRounds = 2;
constexpr uint32_t kSnapshotReadPasses = 2;
constexpr uint32_t kPlannedSnapshotReadPasses = 1;
constexpr size_t kMaximumComponentPagePlans = 512;

struct RuntimeTransform
{
    float rotation[4];
    float translation[3];
    float scale[3];
    uint32_t trailer[2];
};

static_assert(sizeof(RuntimeTransform) == 0x30);
static_assert(offsetof(RuntimeTransform, translation) == 0x10);
static_assert(offsetof(RuntimeTransform, scale) == 0x1C);

using SnapshotPage = std::array<uint8_t, kPageSize>;
using RemoteSnapshot = std::unordered_map<uint64_t, SnapshotPage>;

enum class InstrumentationMode
{
    Full,
    Precise,
};

struct ComponentPagePlan
{
    uint64_t environmentEpoch = 0;
    std::unordered_set<uint64_t> sourcePages;
    uint64_t observedSeekResult = 0;
    bool requiresFullInstrumentation = false;
};

std::mutex gRemoteCaptureMutex;
std::mutex gPagePlanMutex;
std::unordered_map<uint64_t, ComponentPagePlan> gComponentPagePlans;

bool LoadComponentPagePlan(const DecryptTask &task, ComponentPagePlan &plan)
{
    std::lock_guard<std::mutex> lock(gPagePlanMutex);
    const auto found = gComponentPagePlans.find(task.sceneComponent);
    if (found == gComponentPagePlans.end() ||
        found->second.environmentEpoch != task.environmentEpoch ||
        found->second.sourcePages.empty())
        return false;
    plan = found->second;
    return true;
}

void StoreComponentPagePlan(const DecryptTask &task,
                            std::unordered_set<uint64_t> sourcePages,
                            uint64_t observedSeekResult,
                            bool requiresFullInstrumentation)
{
    if (sourcePages.empty())
        return;
    std::lock_guard<std::mutex> lock(gPagePlanMutex);
    if (gComponentPagePlans.size() >= kMaximumComponentPagePlans &&
        !gComponentPagePlans.contains(task.sceneComponent))
        gComponentPagePlans.clear();
    gComponentPagePlans[task.sceneComponent] = {
        task.environmentEpoch,
        std::move(sourcePages),
        observedSeekResult,
        requiresFullInstrumentation,
    };
}

void InvalidateComponentPagePlan(const DecryptTask &task)
{
    std::lock_guard<std::mutex> lock(gPagePlanMutex);
    const auto found = gComponentPagePlans.find(task.sceneComponent);
    if (found != gComponentPagePlans.end() &&
        found->second.environmentEpoch == task.environmentEpoch)
        gComponentPagePlans.erase(found);
}

uint64_t ElapsedMicroseconds(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

bool IsSyntheticStackRange(uint64_t address, uint64_t size)
{
    return size <= kStackSize && address >= kStackBase &&
           address <= kStackBase + kStackSize - size;
}

uint64_t ReadCntfrqEl0()
{
#if defined(__aarch64__)
    uint64_t value = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

uint64_t ReadCtrEl0()
{
#if defined(__aarch64__)
    uint64_t value = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

uint64_t ReadCntvctEl0()
{
#if defined(__aarch64__)
    uint64_t value = 0;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}
bool CaptureRemoteSnapshot(const std::unordered_set<uint64_t> &sourcePages,
                           RemoteSnapshot &snapshot, uint32_t &changedPages,
                           uint32_t readPasses)
{
    if (sourcePages.empty() || readPasses == 0)
        return false;

    RemoteSnapshot previous;
    snapshot.clear();
    changedPages = 0;
    for (uint32_t pass = 0; pass < readPasses; ++pass)
    {
        RemoteSnapshot current;
        current.reserve(sourcePages.size());
        for (uint64_t sourcePage : sourcePages)
        {
            SnapshotPage page{};
            if (!CoordDriverBridge::Read(sourcePage, page.data(), page.size()))
                return false;
            current.emplace(sourcePage, std::move(page));
        }
        if (!previous.empty())
        {
            changedPages = 0;
            for (const auto &[sourcePage, page] : current)
            {
                const auto found = previous.find(sourcePage);
                if (found == previous.end() || found->second != page)
                    ++changedPages;
            }
        }
        previous = std::move(current);
    }
    snapshot = std::move(previous);
    return true;
}
bool IsCoordinateValid(const Coordinate &coordinate)
{
    constexpr float kMaximumCoordinateMagnitude = 20000000.0f;
    return std::isfinite(coordinate.x) && std::isfinite(coordinate.y) &&
           std::isfinite(coordinate.z) && std::fabs(coordinate.x) <= kMaximumCoordinateMagnitude &&
           std::fabs(coordinate.y) <= kMaximumCoordinateMagnitude &&
           std::fabs(coordinate.z) <= kMaximumCoordinateMagnitude;
}

bool IsValidLinuxFileDescriptor(uint64_t fileDescriptor)
{
    return fileDescriptor <= kMaximumLinuxFileDescriptor;
}

const char *ValidateTransform(const RuntimeTransform &transform, bool decrypted)
{
    for (float value : transform.rotation)
        if (!std::isfinite(value))
            return "final Transform rotation is non-finite";
    for (float value : transform.translation)
        if (!std::isfinite(value))
            return "final Transform translation is non-finite";
    for (float value : transform.scale)
        if (!std::isfinite(value))
            return "final Transform scale is non-finite";

    constexpr float kNearZero = 0.000001f;
    bool allZero = true;
    for (float value : transform.rotation)
        allZero = allZero && std::fabs(value) <= kNearZero;
    for (float value : transform.translation)
        allZero = allZero && std::fabs(value) <= kNearZero;
    for (float value : transform.scale)
        allZero = allZero && std::fabs(value) <= kNearZero;
    if (allZero)
        return "final Transform is all zero";

    double quaternionNormSquared = 0.0;
    for (float value : transform.rotation)
        quaternionNormSquared += static_cast<double>(value) * static_cast<double>(value);
    if (quaternionNormSquared <= 0.00000001)
        return "final Transform has a zero quaternion";

    if (std::fabs(transform.scale[0]) <= kNearZero &&
        std::fabs(transform.scale[1]) <= kNearZero &&
        std::fabs(transform.scale[2]) <= kNearZero)
        return "final Transform has an all-zero scale";

    constexpr float kIdentityTolerance = 0.001f;
    const bool nearIdentity =
        std::fabs(transform.rotation[0]) <= kIdentityTolerance &&
        std::fabs(transform.rotation[1]) <= kIdentityTolerance &&
        std::fabs(transform.rotation[2]) <= kIdentityTolerance &&
        std::fabs(transform.rotation[3] - 1.0f) <= kIdentityTolerance &&
        std::fabs(transform.translation[0]) <= kIdentityTolerance &&
        std::fabs(transform.translation[1]) <= kIdentityTolerance &&
        std::fabs(transform.translation[2]) <= kIdentityTolerance &&
        std::fabs(transform.scale[0] - 1.0f) <= kIdentityTolerance &&
        std::fabs(transform.scale[1] - 1.0f) <= kIdentityTolerance &&
        std::fabs(transform.scale[2] - 1.0f) <= kIdentityTolerance;
    if (decrypted && nearIdentity)
        return "decryption returned the observed near-identity Transform sentinel";
    return nullptr;
}
#if defined(JIEMI_WITH_UNICORN)
class GetterExecution
{
public:
    GetterExecution(uint64_t getterAddress, uint64_t sceneComponent,
                    CoordDriverBridge::Environment environment,
                    RingServiceLayout ringLayout,
                    const RemoteSnapshot *snapshot = nullptr,
                    uint64_t seekResultOverride = 0,
                    InstrumentationMode instrumentationMode = InstrumentationMode::Full)
        : getterAddress_(getterAddress), sceneComponent_(sceneComponent), environment_(environment),
          ringLayout_(ringLayout), snapshot_(snapshot), seekResultOverride_(seekResultOverride),
          hasSeekResultOverride_(snapshot != nullptr),
          instrumentationMode_(instrumentationMode)
    {
    }

    ~GetterExecution()
    {
        if (engine_)
            uc_close(engine_);
    }

    bool Run(Coordinate &coordinate)
    {
        coordinate = {};
        if (!Initialize())
            return false;

        uint64_t startAddress = getterAddress_;
        uc_err runError = UC_ERR_OK;
        do
        {
            retryPatchedInstruction_ = false;
            runError = uc_emu_start(engine_, startAddress, kExitAddress,
                                    kEmulationTimeoutMicroseconds, 0);
            if (retryPatchedInstruction_)
            {
                if (!InvalidateCodeAliases(retryAddress_, sizeof(uint32_t)))
                    break;
                startAddress = retryAddress_;
            }
        } while (retryPatchedInstruction_ && !stoppedForError_);

        uint64_t pc = 0;
        uc_reg_read(engine_, UC_ARM64_REG_PC, &pc);

        if (stoppedForError_)
        {
            SetFailure("execution stopped: " + failureReason_);
            return false;
        }
        if (runError != UC_ERR_OK)
        {
            SetFailure(std::string("uc_emu_start: ") + uc_strerror(runError));
            return false;
        }
        if (!reachedGetterReturn_ || CanonicalAddress(pc) != getterReturnAddress_)
        {
            SetFailure("getter did not reach its final return instruction");
            return false;
        }
        if (finalLr_ != kExitAddress)
        {
            SetFailure("getter final return does not target the synthetic caller");
            return false;
        }

        if (!IsRemoteAddress(finalX0Raw_))
        {
            SetFailure("final getter register X0 is not a valid transform pointer");
            return false;
        }

        RuntimeTransform transform{};
        if (!ReadTransform(finalX0Raw_, transform, finalTransformAddress_))
        {
            SetFailure("unable to read the transform referenced by final X0");
            return false;
        }
        if (const char *transformFailure =
                ValidateTransform(transform, resultMode_ != ResultMode::Passthrough))
        {
            SetFailure(transformFailure);
            return false;
        }
        coordinate = {transform.translation[0], transform.translation[1], transform.translation[2]};
        if (!IsCoordinateValid(coordinate))
        {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                          "invalid final XYZ component=0x%llX x0=0x%llX xyz=(%.9g,%.9g,%.9g) scale=(%.9g,%.9g,%.9g)",
                          static_cast<unsigned long long>(sceneComponent_),
                          static_cast<unsigned long long>(finalX0Raw_),
                          transform.translation[0], transform.translation[1], transform.translation[2],
                          transform.scale[0], transform.scale[1], transform.scale[2]);
            SetFailure(message);
            return false;
        }

        transformAddress_ = finalTransformAddress_;
        return true;
    }

    const std::string &FailureReason() const
    {
        return failureReason_;
    }

    uint64_t TransformAddress() const { return transformAddress_; }
    uint64_t ExecutedInstructions() const { return executedInstructions_; }
    uint64_t EmulatedPacgaCount() const { return emulatedPacgaCount_; }
    uint64_t EmulatedSyscallCount() const { return emulatedSyscallCount_; }
    uint64_t UnknownSyscallCount() const { return unknownSyscallCount_; }
    uint64_t LastUnknownSyscall() const { return lastUnknownSyscall_; }
    uint64_t CntfrqReadCount() const { return cntfrqReadCount_; }
    uint64_t CntvctReadCount() const { return cntvctReadCount_; }
    bool GetterPageUnavailable() const { return getterPageUnavailable_; }
    uint64_t FinalProgramCounter() const { return finalPc_; }
    uint64_t FinalLinkRegister() const { return finalLr_; }
    uint64_t FinalX0Raw() const { return finalX0Raw_; }
    uint64_t TransformWriteCount() const { return transformWriteCount_; }
    uint64_t RingAddress() const { return ringAddress_; }
    uint64_t RingSlot() const { return ringSlot_; }
    uint64_t RingServiceHitCount() const { return ringServiceHitCount_; }
    uint64_t RingCaptureCount() const { return ringCaptureCount_; }
    bool RingOwned() const { return ringOwned_; }
    bool IsDecryptedReference() const { return resultMode_ == ResultMode::DecryptedReference; }
    const char *ResultModeName() const
    {
        switch (resultMode_)
        {
        case ResultMode::Passthrough:
            return "passthrough";
        case ResultMode::DecryptedReference:
            return "decrypted_reference";
        case ResultMode::DecryptedWrite:
            return "decrypted_write";
        }
        return "unknown";
    }
    uint64_t ShellcodeIoctlCount() const { return shellcodeIoctlCount_; }
    bool HasObservedSeekFd() const { return hasObservedSeekFd_; }
    uint64_t ObservedSeekFd() const { return observedSeekFd_; }
    uint64_t ObservedSeekResult() const { return observedSeekResult_; }
    bool HasObservedIoctlFd() const { return hasObservedIoctlFd_; }
    uint64_t ObservedIoctlFd() const { return observedIoctlFd_; }
    const char *SyscallContractName() const
    {
        if (ringProofAccepted_)
            return "ring_service";
        if (ringServiceHitCount_ != 0)
            return "ring_incomplete";
        if (syscallContractViolation_)
            return "violation";
        if (syscallStage_ == SyscallStage::Complete)
            return "complete";
        return syscallStage_ == SyscallStage::Initial ? "none" : "incomplete";
    }
    uint32_t MappedRemotePages() const { return mappedRemotePages_; }
    uint32_t RecoveredInstructions() const { return recoveredInstructions_; }
    uint64_t MissingSnapshotPage() const { return missingSnapshotPage_; }
    std::unordered_set<uint64_t> SourcePages() const
    {
        std::unordered_set<uint64_t> pages;
        pages.reserve(remotePageBackings_.size());
        for (const auto &[sourcePage, _] : remotePageBackings_)
            pages.insert(sourcePage);
        return pages;
    }

private:
    enum class ResultMode
    {
        Passthrough,
        DecryptedReference,
        DecryptedWrite,
    };

    enum class SyscallStage
    {
        Initial,
        GotThreadId,
        SoughtObject,
        Complete,
    };

    bool Initialize()
    {
        static const bool qarmaImplementationValid =
            ValidateQarma5Implementation();
        if (!qarmaImplementationValid)
        {
            SetFailure("QARMA5 known-answer self-test failed");
            return false;
        }

        const uc_err openError = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine_);
        if (openError != UC_ERR_OK)
        {
            SetFailure(std::string("uc_open: ") + uc_strerror(openError));
            return false;
        }

        const uc_err cpuModelError = uc_ctl_set_cpu_model(engine_, UC_CPU_ARM64_MAX);
        if (cpuModelError != UC_ERR_OK)
        {
            SetFailure(std::string("uc_ctl_set_cpu_model: ") + uc_strerror(cpuModelError));
            return false;
        }

        if (!MapPrivatePage(kExitAddress, UC_PROT_ALL) || !MapPrivatePage(kStackBase, UC_PROT_ALL, kStackSize) ||
            !MapRemotePage(getterAddress_))
            return false;
        if (!ringLayout_.valid)
        {
            SetFailure("dynamic ring-service layout is unavailable");
            return false;
        }

        const uint32_t nop = 0xD503201FU;
        if (uc_mem_write(engine_, kExitAddress, &nop, sizeof(nop)) != UC_ERR_OK)
        {
            SetFailure("unable to write the synthetic return page");
            return false;
        }

        uint64_t stackPointer = kStackBase + kStackSize - 0x10;
        if (!WriteRegister(UC_ARM64_REG_X0, sceneComponent_) || !WriteRegister(UC_ARM64_REG_X30, kExitAddress) ||
            !WriteRegister(UC_ARM64_REG_SP, stackPointer) || !WriteRegister(UC_ARM64_REG_PC, getterAddress_) ||
            !WriteRegister(UC_ARM64_REG_TPIDR_EL0, environment_.tls))
            return false;

        if (!ConfigurePacgaKeys())
            return false;

        const bool commonHooksReady =
            uc_hook_add(engine_, &memoryHook_, UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT,
                        reinterpret_cast<void *>(MemoryFaultHook), this, 1, 0) == UC_ERR_OK &&
            uc_hook_add(engine_, &memoryWriteHook_, UC_HOOK_MEM_WRITE,
                        reinterpret_cast<void *>(MemoryWriteHook), this, 1, 0) == UC_ERR_OK &&
            uc_hook_add(engine_, &invalidInstructionHook_, UC_HOOK_INSN_INVALID,
                        reinterpret_cast<void *>(InvalidInstructionHook), this, 1, 0) == UC_ERR_OK &&
            uc_hook_add(engine_, &sysRegisterHook_, UC_HOOK_INSN,
                        reinterpret_cast<void *>(SystemRegisterHook),
                        this, 1, 0, UC_ARM64_INS_MRS) == UC_ERR_OK &&
            uc_hook_add(engine_, &interruptHook_, UC_HOOK_INTR,
                        reinterpret_cast<void *>(InterruptHook), this, 1, 0) == UC_ERR_OK;

        bool executionHooksReady = false;
        if (instrumentationMode_ == InstrumentationMode::Full)
        {
            executionHooksReady =
                uc_hook_add(engine_, &codeHook_, UC_HOOK_CODE,
                            reinterpret_cast<void *>(CodeHook), this, 1, 0) == UC_ERR_OK;
        }
        else
        {
            executionHooksReady =
                uc_hook_add(engine_, &blockHook_, UC_HOOK_BLOCK,
                            reinterpret_cast<void *>(BlockHook), this, 1, 0) == UC_ERR_OK &&
                uc_hook_add(engine_, &getterReturnHook_, UC_HOOK_CODE,
                            reinterpret_cast<void *>(CodeHook), this,
                            getterReturnAddress_, getterReturnAddress_) == UC_ERR_OK &&
                uc_hook_add(engine_, &ringSvcHook_, UC_HOOK_CODE,
                            reinterpret_cast<void *>(CodeHook), this,
                            ringLayout_.svc, ringLayout_.svc) == UC_ERR_OK &&
                uc_hook_add(engine_, &hashEndHook_, UC_HOOK_CODE,
                            reinterpret_cast<void *>(CodeHook), this,
                            ringLayout_.hashEnd, ringLayout_.hashEnd) == UC_ERR_OK;
        }

        if (!commonHooksReady || !executionHooksReady)
        {
            SetFailure("unable to install Unicorn hooks");
            return false;
        }

        return true;
    }

    bool WriteRegister(int registerId, uint64_t value)
    {
        if (uc_reg_write(engine_, registerId, &value) == UC_ERR_OK)
            return true;
        SetFailure("unable to initialize an ARM64 register");
        return false;
    }

    bool ConfigurePacgaKeys()
    {
        uc_arm64_cp_reg keyLow{};
        keyLow.op0 = 3;
        keyLow.op1 = 0;
        keyLow.crn = 2;
        keyLow.crm = 3;
        keyLow.op2 = 0;
        keyLow.val = environment_.pacgaLo;

        uc_arm64_cp_reg keyHigh = keyLow;
        keyHigh.op2 = 1;
        keyHigh.val = environment_.pacgaHi;

        if (uc_reg_write(engine_, UC_ARM64_REG_CP_REG, &keyLow) != UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_CP_REG, &keyHigh) != UC_ERR_OK)
        {
            SetFailure("Unicorn cannot configure APGA keys");
            return false;
        }
        return true;
    }

    bool MapPrivatePage(uint64_t address, uint32_t permissions, uint64_t size = kPageSize)
    {
        if (uc_mem_map(engine_, address, size, permissions) == UC_ERR_OK)
            return true;
        SetFailure("unable to map private Unicorn memory");
        return false;
    }

    bool MapRemotePage(uint64_t address)
    {
        const uint64_t canonical = CanonicalAddress(address);
        if (!IsRemoteAddress(canonical))
        {
            SetFailure("attempted to map an invalid remote address");
            return false;
        }

        const uint64_t sourcePage = canonical & ~(kPageSize - 1);
        const uint64_t accessPage = address & ~(kPageSize - 1);
        if (mappedAccessPages_.contains(accessPage))
            return true;

        auto backing = remotePageBackings_.find(sourcePage);
        if (backing == remotePageBackings_.end())
        {
            if (mappedRemotePages_ >= kRemotePageLimit)
            {
                SetFailure("remote page limit exceeded");
                return false;
            }

            auto page = std::make_unique<std::array<uint8_t, kPageSize>>();
            bool pageReady = false;
            if (snapshot_)
            {
                const auto found = snapshot_->find(sourcePage);
                if (found != snapshot_->end())
                {
                    *page = found->second;
                    pageReady = true;
                }
                else
                {
                    missingSnapshotPage_ = sourcePage;
                    char message[128]{};
                    std::snprintf(message, sizeof(message),
                                  "snapshot is missing remote page 0x%llX",
                                  static_cast<unsigned long long>(sourcePage));
                    SetFailure(message);
                    return false;
                }
            }
            else
            {
                pageReady = CoordDriverBridge::Read(sourcePage, page->data(), page->size());
            }
            if (!pageReady)
            {
                getterPageUnavailable_ = sourcePage == (CanonicalAddress(getterAddress_) & ~(kPageSize - 1));
                char message[160]{};
                std::snprintf(message, sizeof(message),
                              "unable to mirror remote page 0x%llX component=0x%llX getter_page=%s",
                              static_cast<unsigned long long>(sourcePage),
                              static_cast<unsigned long long>(sceneComponent_),
                              getterPageUnavailable_ ? "yes" : "no");
                SetFailure(message);
                return false;
            }
            backing = remotePageBackings_.emplace(sourcePage, std::move(page)).first;
            ++mappedRemotePages_;
        }

        if (!MapMirroredPage(accessPage, *backing->second))
            return false;
        if (accessPage != sourcePage && !MapMirroredPage(sourcePage, *backing->second))
            return false;
        return true;
    }

    bool MapMirroredPage(uint64_t pageAddress, std::array<uint8_t, kPageSize> &page)
    {
        if (mappedAccessPages_.contains(pageAddress))
            return true;

        const uc_err mapError = uc_mem_map_ptr(engine_, pageAddress, kPageSize, UC_PROT_ALL, page.data());
        if (mapError != UC_ERR_OK)
        {
            char message[128]{};
            std::snprintf(message, sizeof(message), "uc_mem_map_ptr(0x%llX): %s",
                          static_cast<unsigned long long>(pageAddress), uc_strerror(mapError));
            SetFailure(message);
            return false;
        }

        mappedAccessPages_.insert(pageAddress);
        return true;
    }

    bool InvalidateCodeAliases(uint64_t address, uint64_t size)
    {
        const uint64_t canonicalAddress = CanonicalAddress(address);
        const uint64_t sourcePage = canonicalAddress & ~(kPageSize - 1);
        const uint64_t pageOffset = canonicalAddress & (kPageSize - 1);
        for (uint64_t accessPage : mappedAccessPages_)
        {
            if (CanonicalAddress(accessPage) != sourcePage)
                continue;
            const uint64_t aliasAddress = accessPage + pageOffset;
            const uc_err cacheError = uc_ctl_remove_cache(engine_, aliasAddress, aliasAddress + size);
            if (cacheError != UC_ERR_OK)
            {
                SetFailure(std::string("unable to invalidate modified ARM64 code: ") + uc_strerror(cacheError));
                stoppedForError_ = true;
                return false;
            }
        }
        return true;
    }

    bool ReadTransform(uint64_t transformAddress, RuntimeTransform &transform, uint64_t &readAddress)
    {
        const uint64_t canonicalAddress = CanonicalAddress(transformAddress);
        const uint64_t xyzStart = canonicalAddress + offsetof(RuntimeTransform, translation);
        const uint64_t xyzEnd = xyzStart + sizeof(transform.translation);
        uint16_t xyzWriteMask = 0;
        for (const MemoryWrite &write : memoryWrites_)
        {
            const uint64_t writeEnd = write.canonicalAddress + write.size;
            const uint64_t overlapStart = std::max(write.canonicalAddress, xyzStart);
            const uint64_t overlapEnd = std::min(writeEnd, xyzEnd);
            if (overlapStart >= overlapEnd)
                continue;
            for (uint64_t byte = overlapStart; byte < overlapEnd; ++byte)
                xyzWriteMask |= static_cast<uint16_t>(1U << (byte - xyzStart));
        }

        constexpr uint16_t kFullXyzMask = (1U << sizeof(transform.translation)) - 1U;
        const uint64_t expectedTransform = CanonicalAddress(sceneComponent_ + 0x210);
        const bool cleanPassthrough = emulatedPacgaCount_ == 0 && emulatedSyscallCount_ == 0 &&
                                      unknownSyscallCount_ == 0 && !syscallContractViolation_ &&
                                      syscallStage_ == SyscallStage::Initial;
        const bool syscallProvenDecryption =
            emulatedPacgaCount_ != 0 && emulatedSyscallCount_ == 3 &&
            shellcodeIoctlCount_ == 1 && unknownSyscallCount_ == 0 &&
            !syscallContractViolation_ && syscallStage_ == SyscallStage::Complete;
        ringOwned_ = false;
        ringSlot_ = 0;
        if (ringLayout_.valid && ringServiceHitCount_ == 1 && ringCaptureCount_ == 1 &&
            canonicalAddress >= ringAddress_ + ringLayout_.ringOffset)
        {
            const uint64_t delta = canonicalAddress - ringAddress_ - ringLayout_.ringOffset;
            if (ringLayout_.ringStride != 0 && delta % ringLayout_.ringStride == 0)
            {
                const uint64_t slot = delta / ringLayout_.ringStride;
                if (slot < kMaximumRingSlot)
                {
                    ringSlot_ = slot;
                    ringOwned_ = true;
                }
            }
        }
        ringProofAccepted_ =
            ringOwned_ && emulatedPacgaCount_ != 0 && emulatedSyscallCount_ == 0 &&
            unknownSyscallCount_ == 0 && !syscallContractViolation_ &&
            syscallStage_ == SyscallStage::Initial;
        const bool provenDecryption = syscallProvenDecryption || ringProofAccepted_;
        if (xyzWriteMask == kFullXyzMask)
        {
            if (!provenDecryption)
            {
                SetFailure("final XYZ write is not backed by the complete decryption syscall contract");
                return false;
            }
            transformWriteCount_ = sizeof(transform.translation);
            resultMode_ = ResultMode::DecryptedWrite;
        }
        else if (xyzWriteMask != 0)
        {
            SetFailure("shellcode only partially wrote the final XYZ result");
            return false;
        }
        else if (cleanPassthrough && canonicalAddress == expectedTransform)
        {
            resultMode_ = ResultMode::Passthrough;
        }
        else if (provenDecryption && canonicalAddress != expectedTransform)
        {
            resultMode_ = ResultMode::DecryptedReference;
        }
        else
        {
            SetFailure("final Transform is neither a proven passthrough nor a proven decrypted result");
            return false;
        }

        readAddress = transformAddress;
        if (!MapRemotePage(readAddress))
            return false;
        const uint64_t transformEnd = readAddress + sizeof(transform) - 1;
        if (transformEnd < readAddress || !MapRemotePage(transformEnd))
            return false;
        return uc_mem_read(engine_, readAddress, &transform, sizeof(transform)) == UC_ERR_OK;
    }

    void HandleMemoryWrite(uint64_t address, int size)
    {
        if (size <= 0 || !IsRemoteAddress(address))
            return;
        const uint64_t canonicalAddress = CanonicalAddress(address);
        const uint64_t writeSize = static_cast<uint64_t>(size);
        const uint64_t writeEnd =
            canonicalAddress <= UINT64_MAX - writeSize
                ? canonicalAddress + writeSize
                : UINT64_MAX;
        bool relevantWrite = false;
        if (ringAddress_ != 0 && ringLayout_.ringStride != 0)
        {
            const uint64_t ringDataStart = ringAddress_ + ringLayout_.ringOffset;
            const uint64_t ringDataSize =
                ringLayout_.ringStride * kMaximumRingSlot + sizeof(RuntimeTransform);
            const uint64_t ringDataEnd = ringDataStart + ringDataSize;
            relevantWrite =
                writeEnd > ringDataStart && canonicalAddress < ringDataEnd;
        }
        else
        {
            const uint64_t componentTransform =
                CanonicalAddress(sceneComponent_ + 0x210);
            relevantWrite =
                (writeEnd > componentTransform &&
                 canonicalAddress < componentTransform + sizeof(RuntimeTransform)) ||
                emulatedSyscallCount_ != 0;
        }
        if (!relevantWrite)
            return;
        if (memoryWrites_.size() < 65536)
            memoryWrites_.push_back({address, canonicalAddress, static_cast<uint32_t>(size)});
    }

    bool HandleMemoryFault(uint64_t address)
    {
        if (MapRemotePage(address))
            return true;
        stoppedForError_ = true;
        uc_emu_stop(engine_);
        return false;
    }

    void HandleBlock(uint32_t size)
    {
        const uint64_t instructionCount = std::max<uint64_t>(1, size / sizeof(uint32_t));
        executedInstructions_ += instructionCount;
        if (executedInstructions_ <= kInstructionLimit)
            return;
        SetFailure("instruction limit exceeded");
        stoppedForError_ = true;
        uc_emu_stop(engine_);
    }

    bool ResolveRingServiceTarget(uint64_t &target,
                                  bool &injectState)
    {
        target = 0;
        injectState = false;
        uint64_t stackPointer = 0;
        std::array<uint8_t, 0x800> stack{};
        if (uc_reg_read(engine_, UC_ARM64_REG_SP,
                        &stackPointer) != UC_ERR_OK ||
            !IsSyntheticStackRange(stackPointer, stack.size()) ||
            uc_mem_read(engine_, stackPointer, stack.data(),
                        stack.size()) != UC_ERR_OK)
        {
            SetFailure("unable to inspect ring service stack parameters");
            return false;
        }
        const uint64_t expectedComponentTransform =
            CanonicalAddress(sceneComponent_ + 0x210);
        const auto stackMatches =
            [&](uint32_t offset)
        {
            if (offset > stack.size() - sizeof(uint64_t))
                return false;
            uint64_t value = 0;
            std::memcpy(&value, stack.data() + offset,
                        sizeof(value));
            return CanonicalAddress(value) ==
                   expectedComponentTransform;
        };

        uint32_t selector = 0;
        for (const RingStateGuard &guard : ringLayout_.stateGuards)
        {
            if (!stackMatches(guard.componentStackOffset))
                continue;
            if (selector != 0 && selector != guard.selector)
            {
                SetFailure("ring service component-state guard is ambiguous");
                return false;
            }
            selector = guard.selector;
        }

        uint64_t legacyEntry = 0;
        for (const RingLegacyGuard &guard : ringLayout_.legacyGuards)
        {
            if (!stackMatches(guard.componentStackOffset))
                continue;
            if (legacyEntry != 0 && legacyEntry != guard.entry)
            {
                SetFailure("ring service legacy component guard is ambiguous");
                return false;
            }
            legacyEntry = guard.entry;
        }

        if (selector != 0 && legacyEntry != 0)
        {
            SetFailure("ring service component matches both dispatcher and legacy guards");
            return false;
        }
        if (selector != 0)
        {
            ringLayout_.stateSelector = selector;
            target = ringLayout_.dispatcher;
            injectState = true;
            return true;
        }
        if (legacyEntry != 0)
        {
            target = legacyEntry;
            return true;
        }
        SetFailure("unable to match component transform to a ring service guard");
        return false;
    }

    void HandleCode(uint64_t address)
    {
        if (instrumentationMode_ == InstrumentationMode::Full)
        {
            ++executedInstructions_;
            if (executedInstructions_ > kInstructionLimit)
            {
                SetFailure("instruction limit exceeded");
                stoppedForError_ = true;
                uc_emu_stop(engine_);
                return;
            }
        }

        uint32_t instruction = 0;
        if (uc_mem_read(engine_, address, &instruction, sizeof(instruction)) != UC_ERR_OK)
            return;
        lastCodeAddress_ = address;
        lastCodeInstruction_ = instruction;

        constexpr uint32_t kReturnX30 = 0xD65F03C0U;
        if (CanonicalAddress(address) == getterReturnAddress_ && instruction == kReturnX30)
        {
            if (uc_reg_read(engine_, UC_ARM64_REG_X0, &finalX0Raw_) != UC_ERR_OK ||
                uc_reg_read(engine_, UC_ARM64_REG_X30, &finalLr_) != UC_ERR_OK)
            {
                SetFailure("unable to capture final getter registers");
                stoppedForError_ = true;
            }
            else
            {
                finalPc_ = CanonicalAddress(address);
                finalLr_ = CanonicalAddress(finalLr_);
                reachedGetterReturn_ = true;
            }
            uc_emu_stop(engine_);
            return;
        }

        const uint64_t canonicalAddress = CanonicalAddress(address);
        if (ringLayout_.valid && ringLayout_.hashEnd != 0 &&
            canonicalAddress == ringLayout_.hashEnd)
        {
            uint64_t ring = 0;
            const int ringRegister = Arm64XRegister(ringLayout_.hashRegister);
            if (ringRegister == UC_ARM64_REG_INVALID ||
                uc_reg_read(engine_, ringRegister, &ring) != UC_ERR_OK ||
                !IsRemoteAddress(CanonicalAddress(ring)))
            {
                SetFailure("unable to capture a valid component ring");
                stoppedForError_ = true;
                uc_emu_stop(engine_);
                return;
            }
            ringAddress_ = CanonicalAddress(ring);
            ++ringCaptureCount_;
        }

        if (ringLayout_.valid && canonicalAddress == ringLayout_.svc &&
            instruction == kSvcInstruction)
        {
            if (ringServiceHitCount_ != 0)
            {
                SetFailure("ring service reentered its dynamic SVC");
                stoppedForError_ = true;
                uc_emu_stop(engine_);
                return;
            }
            uint64_t target = 0;
            bool injectState = false;
            if (!ResolveRingServiceTarget(target, injectState))
            {
                stoppedForError_ = true;
                uc_emu_stop(engine_);
                return;
            }
            if (injectState)
            {
                uint64_t stackPointer = 0;
                if (uc_reg_read(engine_, UC_ARM64_REG_SP,
                                &stackPointer) != UC_ERR_OK ||
                    uc_mem_write(engine_,
                                 stackPointer + ringLayout_.stateSlotOffset,
                                 &ringLayout_.stateSelector,
                                 sizeof(ringLayout_.stateSelector)) != UC_ERR_OK)
                {
                    SetFailure("unable to install ring service state selector");
                    stoppedForError_ = true;
                    uc_emu_stop(engine_);
                    return;
                }
            }
            if (uc_reg_write(engine_, UC_ARM64_REG_PC, &target) != UC_ERR_OK)
            {
                SetFailure("unable to dispatch the dynamic ring service");
                stoppedForError_ = true;
                uc_emu_stop(engine_);
                return;
            }
            ++ringServiceHitCount_;
            return;
        }

        if (instruction == kSvcInstruction && HandleLinuxSystemCall(address))
            return;

        constexpr uint32_t kBranchRegisterMask = 0xFFFFFC1FU;
        constexpr uint32_t kBranchRegister = 0xD61F0000U;
        constexpr uint32_t kBranchLinkRegister = 0xD63F0000U;
        constexpr uint32_t kReturnRegister = 0xD65F0000U;
        const uint32_t opcode = instruction & kBranchRegisterMask;
        if (opcode != kBranchRegister && opcode != kBranchLinkRegister && opcode != kReturnRegister)
            return;

        const uint32_t registerIndex = (instruction >> 5) & 0x1FU;
        if (registerIndex >= 31)
            return;

        uint64_t target = 0;
        const int registerId = Arm64XRegister(registerIndex);
        if (registerId == UC_ARM64_REG_INVALID)
            return;
        if (uc_reg_read(engine_, registerId, &target) != UC_ERR_OK)
            return;

        const uint64_t canonicalTarget = CanonicalAddress(target);
        if (target != canonicalTarget)
            uc_reg_write(engine_, registerId, &canonicalTarget);
    }

    bool HandleLinuxSystemCall(uint64_t address)
    {
        uint64_t syscallNumber = 0;
        uint64_t argument0 = 0;
        uint64_t argument1 = 0;
        uint64_t argument2 = 0;
        if (uc_reg_read(engine_, UC_ARM64_REG_X8, &syscallNumber) != UC_ERR_OK ||
            uc_reg_read(engine_, UC_ARM64_REG_X0, &argument0) != UC_ERR_OK ||
            uc_reg_read(engine_, UC_ARM64_REG_X1, &argument1) != UC_ERR_OK ||
            uc_reg_read(engine_, UC_ARM64_REG_X2, &argument2) != UC_ERR_OK)
            return false;

        uint64_t result = 0;
        switch (syscallNumber)
        {
        case 178: // gettid must match the GameThread whose TLS/APGA environment was captured.
            if (syscallStage_ == SyscallStage::Initial)
            {
                if (environment_.tid == 0)
                {
                    result = static_cast<uint64_t>(-3LL); // -ESRCH
                    syscallContractViolation_ = true;
                    break;
                }
                result = environment_.tid;
                syscallStage_ = SyscallStage::GotThreadId;
            }
            else
            {
                result = static_cast<uint64_t>(-38LL); // -ENOSYS
                syscallContractViolation_ = true;
            }
            break;
        case 62: // lseek(fd, 0, SEEK_END); result is the current target fd object's live size.
            observedSeekFd_ = argument0;
            hasObservedSeekFd_ = true;
            if (syscallStage_ == SyscallStage::GotThreadId && IsValidLinuxFileDescriptor(argument0) &&
                argument1 == 0 && argument2 == 2)
            {
                uint64_t fileSize = 0;
                const bool sizeReady =
                    hasSeekResultOverride_
                        ? (fileSize = seekResultOverride_, true)
                        : CoordDriverBridge::GetFileDescriptorSize(
                              environment_.processId, argument0, fileSize);
                if (sizeReady)
                {
                    result = fileSize;
                    observedSeekResult_ = fileSize;
                    syscallStage_ = SyscallStage::SoughtObject;
                }
                else
                {
                    result = static_cast<uint64_t>(-9LL); // -EBADF
                    syscallContractViolation_ = true;
                }
            }
            else
            {
                result = static_cast<uint64_t>(-22LL); // -EINVAL
                syscallContractViolation_ = true;
            }
            break;
        case 29: // Live KGSL ioctl returns -EINVAL; the obfuscated caller consumes the raw syscall result.
            observedIoctlFd_ = argument0;
            hasObservedIoctlFd_ = true;
            if (syscallStage_ == SyscallStage::SoughtObject && IsValidLinuxFileDescriptor(argument0) &&
                argument1 == kShellcodeIoctlCommand &&
                IsSyntheticStackRange(argument2, kShellcodeIoctlBufferSize))
            {
                result = static_cast<uint64_t>(kShellcodeIoctlResult);
                ++shellcodeIoctlCount_;
                syscallStage_ = SyscallStage::Complete;
                break;
            }
            result = static_cast<uint64_t>(-38LL); // -ENOSYS
            syscallContractViolation_ = true;
            break;
        default:
            lastUnknownSyscall_ = syscallNumber;
            ++unknownSyscallCount_;
            {
                char message[128]{};
                std::snprintf(message, sizeof(message),
                              "unexpected Linux syscall %llu",
                              static_cast<unsigned long long>(syscallNumber));
                SetFailure(message);
            }
            stoppedForError_ = true;
            uc_emu_stop(engine_);
            return true;
        }

        const uint64_t nextPc = address + sizeof(uint32_t);
        if (uc_reg_write(engine_, UC_ARM64_REG_X0, &result) != UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_PC, &nextPc) != UC_ERR_OK)
            return false;
        ++emulatedSyscallCount_;
        return true;
    }

    static int Arm64XRegister(uint32_t registerIndex)
    {
        if (registerIndex <= 28)
            return UC_ARM64_REG_X0 + static_cast<int>(registerIndex);
        if (registerIndex == 29)
            return UC_ARM64_REG_X29;
        if (registerIndex == 30)
            return UC_ARM64_REG_X30;
        return UC_ARM64_REG_INVALID;
    }

    bool HandleInvalidInstruction()
    {
        uint64_t pc = 0;
        uint32_t instruction = 0;
        uc_reg_read(engine_, UC_ARM64_REG_PC, &pc);
        uc_mem_read(engine_, pc, &instruction, sizeof(instruction));

        constexpr uint32_t kPacgaMask = 0xFFE0FC00U;
        constexpr uint32_t kPacgaOpcode = 0x9AC03000U;
        if ((instruction & kPacgaMask) == kPacgaOpcode)
            SetFailure("Unicorn rejected PACGA after APGA key setup");
        else
            SetFailure("Unicorn rejected an ARM64 instruction");
        stoppedForError_ = true;
        return false;
    }

    uint32_t HandleSystemRegister(uc_arm64_reg destination, const uc_arm64_cp_reg *registerInfo)
    {
        if (!registerInfo)
            return false;

        const bool isTpidrEl0 = registerInfo->op0 == 3 && registerInfo->op1 == 3 &&
                                 registerInfo->crn == 13 && registerInfo->crm == 0 &&
                                 registerInfo->op2 == 2;
        const bool isCtrEl0 = registerInfo->op0 == 3 && registerInfo->op1 == 3 &&
                              registerInfo->crn == 0 && registerInfo->crm == 0 &&
                              registerInfo->op2 == 1;
        const bool isCntfrqEl0 = registerInfo->op0 == 3 && registerInfo->op1 == 3 &&
                                 registerInfo->crn == 14 && registerInfo->crm == 0 &&
                                 registerInfo->op2 == 0;
        const bool isCntvctEl0 = registerInfo->op0 == 3 && registerInfo->op1 == 3 &&
                                 registerInfo->crn == 14 && registerInfo->crm == 0 &&
                                 registerInfo->op2 == 2;
        if (!isTpidrEl0 && !isCtrEl0 && !isCntfrqEl0 && !isCntvctEl0)
            return false;

        uint64_t value = 0;
        const char *registerName = nullptr;
        if (isTpidrEl0)
        {
            value = environment_.tls;
            registerName = "TPIDR_EL0";
        }
        else if (isCtrEl0)
        {
            value = ReadCtrEl0();
            registerName = "CTR_EL0";
        }
        else if (isCntfrqEl0)
        {
            value = ReadCntfrqEl0();
            registerName = "CNTFRQ_EL0";
            ++cntfrqReadCount_;
        }
        else
        {
            value = ReadCntvctEl0();
            registerName = "CNTVCT_EL0";
            ++cntvctReadCount_;
        }
        if (uc_reg_write(engine_, destination, &value) != UC_ERR_OK)
        {
            SetFailure(std::string("unable to emulate MRS ") + registerName);
            stoppedForError_ = true;
            uc_emu_stop(engine_);
            return false;
        }
        return true;
    }

    void HandleInterrupt(uint32_t interruptNumber)
    {
        uint64_t pc = 0;
        uint64_t x0 = 0;
        uint64_t x8 = 0;
        uint32_t instruction = 0;
        uint32_t previousInstruction = 0;
        uc_reg_read(engine_, UC_ARM64_REG_PC, &pc);
        uc_reg_read(engine_, UC_ARM64_REG_X0, &x0);
        uc_reg_read(engine_, UC_ARM64_REG_X8, &x8);
        uc_mem_read(engine_, pc, &instruction, sizeof(instruction));
        if (pc >= sizeof(previousInstruction))
            uc_mem_read(engine_, pc - sizeof(previousInstruction), &previousInstruction, sizeof(previousInstruction));

        uint64_t faultAddress = pc;
        uint32_t faultInstruction = instruction;
        if (lastCodeAddress_ != 0 &&
            (lastCodeAddress_ == pc || lastCodeAddress_ + sizeof(uint32_t) == pc))
        {
            faultAddress = lastCodeAddress_;
            faultInstruction = lastCodeInstruction_;
        }
        else if (interruptNumber == 1 && (previousInstruction == 0 || previousInstruction == UINT32_MAX))
        {
            faultAddress = pc - sizeof(previousInstruction);
            faultInstruction = previousInstruction;
        }

        constexpr uint32_t kPacgaMask = 0xFFE0FC00U;
        constexpr uint32_t kPacgaOpcode = 0x9AC03000U;
        uint64_t svcAddress = 0;
        if (instruction == kSvcInstruction)
            svcAddress = pc;
        else if (previousInstruction == kSvcInstruction && pc >= sizeof(previousInstruction))
            svcAddress = pc - sizeof(previousInstruction);
        if (svcAddress != 0)
        {
            if (ringLayout_.valid && CanonicalAddress(svcAddress) == ringLayout_.svc)
            {
                if (ringServiceHitCount_ != 0)
                {
                    SetFailure("ring service reentered its dynamic SVC");
                    stoppedForError_ = true;
                    uc_emu_stop(engine_);
                    return;
                }
                uint64_t target = 0;
                bool injectState = false;
                if (!ResolveRingServiceTarget(target, injectState))
                {
                    stoppedForError_ = true;
                    uc_emu_stop(engine_);
                    return;
                }
                if (injectState)
                {
                    uint64_t stackPointer = 0;
                    if (uc_reg_read(engine_, UC_ARM64_REG_SP,
                                    &stackPointer) != UC_ERR_OK ||
                        uc_mem_write(
                            engine_,
                            stackPointer + ringLayout_.stateSlotOffset,
                            &ringLayout_.stateSelector,
                            sizeof(ringLayout_.stateSelector)) != UC_ERR_OK)
                    {
                        SetFailure("unable to install ring service state selector");
                        stoppedForError_ = true;
                        uc_emu_stop(engine_);
                        return;
                    }
                }
                if (uc_reg_write(engine_, UC_ARM64_REG_PC, &target) == UC_ERR_OK)
                {
                    ++ringServiceHitCount_;
                    return;
                }
            }
            else if (HandleLinuxSystemCall(svcAddress))
            {
                return;
            }
        }
        if ((faultInstruction & kPacgaMask) != kPacgaOpcode &&
            (previousInstruction & kPacgaMask) == kPacgaOpcode &&
            pc >= sizeof(previousInstruction))
        {
            faultAddress = pc - sizeof(previousInstruction);
            faultInstruction = previousInstruction;
        }
        const uint32_t pacgaInstruction = faultInstruction;
        const uint64_t nextPc = faultAddress + sizeof(faultInstruction);
        if (interruptNumber == 1 && (pacgaInstruction & kPacgaMask) == kPacgaOpcode)
        {
            const uint32_t destinationIndex = pacgaInstruction & 0x1FU;
            const uint32_t dataIndex = (pacgaInstruction >> 5) & 0x1FU;
            const uint32_t modifierIndex = (pacgaInstruction >> 16) & 0x1FU;
            const int destinationRegister = Arm64XRegister(destinationIndex);
            const int dataRegister = Arm64XRegister(dataIndex);
            const int modifierRegister = Arm64XRegister(modifierIndex);
            if (destinationRegister != UC_ARM64_REG_INVALID && dataRegister != UC_ARM64_REG_INVALID &&
                modifierRegister != UC_ARM64_REG_INVALID)
            {
                uint64_t data = 0;
                uint64_t modifier = 0;
                if (uc_reg_read(engine_, dataRegister, &data) == UC_ERR_OK &&
                    uc_reg_read(engine_, modifierRegister, &modifier) == UC_ERR_OK)
                {
                    const ApgaKey key{environment_.pacgaLo, environment_.pacgaHi};
                    const uint64_t result = ComputeQarma5Pac(data, modifier, key) & UINT64_C(0xFFFFFFFF00000000);
                    if (uc_reg_write(engine_, destinationRegister, &result) == UC_ERR_OK &&
                        uc_reg_write(engine_, UC_ARM64_REG_PC, &nextPc) == UC_ERR_OK)
                    {
                        ++emulatedPacgaCount_;
                        return;
                    }
                }
            }
        }

        if (interruptNumber == 1 && (faultInstruction == 0 || faultInstruction == UINT32_MAX) &&
            recoveredInstructions_ < kInstructionRecoveryLimit)
        {
            uint32_t remoteInstruction = 0;
            const uint64_t canonicalPc = CanonicalAddress(faultAddress);
            bool instructionReady = false;
            if (snapshot_)
            {
                const uint64_t sourcePage = canonicalPc & ~(kPageSize - 1);
                const auto found = snapshot_->find(sourcePage);
                const uint64_t offset = canonicalPc - sourcePage;
                if (found != snapshot_->end() &&
                    offset <= kPageSize - sizeof(remoteInstruction))
                {
                    std::memcpy(&remoteInstruction, found->second.data() + offset,
                                sizeof(remoteInstruction));
                    instructionReady = true;
                }
            }
            else
            {
                instructionReady =
                    CoordDriverBridge::Read(canonicalPc, &remoteInstruction,
                                            sizeof(remoteInstruction));
            }
            if (instructionReady &&
                remoteInstruction != 0 && remoteInstruction != UINT32_MAX &&
                uc_mem_write(engine_, faultAddress, &remoteInstruction, sizeof(remoteInstruction)) == UC_ERR_OK)
            {
                ++recoveredInstructions_;
                retryAddress_ = faultAddress;
                retryPatchedInstruction_ = true;
                uc_emu_stop(engine_);
                return;
            }
        }

        char message[256]{};
        std::snprintf(message, sizeof(message),
                  "unexpected ARM64 interrupt %u component=0x%llX pc=0x%llX insn=0x%08X prev=0x%08X fault_pc=0x%llX fault_insn=0x%08X last_code=0x%llX x0=0x%llX x8=0x%llX",
                      interruptNumber, static_cast<unsigned long long>(sceneComponent_),
                      static_cast<unsigned long long>(pc), instruction,
                  previousInstruction, static_cast<unsigned long long>(faultAddress), faultInstruction,
                  static_cast<unsigned long long>(lastCodeAddress_), static_cast<unsigned long long>(x0),
                      static_cast<unsigned long long>(x8));
        SetFailure(message);
        stoppedForError_ = true;
        uc_emu_stop(engine_);
    }

    void SetFailure(std::string reason)
    {
        if (failureReason_.empty())
            failureReason_ = std::move(reason);
    }

    static bool MemoryFaultHook(uc_engine *, uc_mem_type, uint64_t address, int, int64_t, void *userData)
    {
        return static_cast<GetterExecution *>(userData)->HandleMemoryFault(address);
    }

    static void MemoryWriteHook(uc_engine *, uc_mem_type, uint64_t address, int size, int64_t, void *userData)
    {
        static_cast<GetterExecution *>(userData)->HandleMemoryWrite(address, size);
    }

    static void CodeHook(uc_engine *, uint64_t address, uint32_t, void *userData)
    {
        static_cast<GetterExecution *>(userData)->HandleCode(address);
    }

    static void BlockHook(uc_engine *, uint64_t, uint32_t size, void *userData)
    {
        static_cast<GetterExecution *>(userData)->HandleBlock(size);
    }

    static bool InvalidInstructionHook(uc_engine *, void *userData)
    {
        return static_cast<GetterExecution *>(userData)->HandleInvalidInstruction();
    }

    static uint32_t SystemRegisterHook(uc_engine *, uc_arm64_reg destination,
                                       const uc_arm64_cp_reg *registerInfo, void *userData)
    {
        return static_cast<GetterExecution *>(userData)->HandleSystemRegister(destination, registerInfo);
    }

    static void InterruptHook(uc_engine *, uint32_t interruptNumber, void *userData)
    {
        static_cast<GetterExecution *>(userData)->HandleInterrupt(interruptNumber);
    }

    uc_engine *engine_ = nullptr;
    uint64_t getterAddress_ = 0;
    uint64_t getterReturnAddress_ = getterAddress_ + 0x24;
    uint64_t sceneComponent_ = 0;
    CoordDriverBridge::Environment environment_{};
    RingServiceLayout ringLayout_{};
    const RemoteSnapshot *snapshot_ = nullptr;
    uint64_t seekResultOverride_ = 0;
    uint64_t missingSnapshotPage_ = 0;
    bool hasSeekResultOverride_ = false;
    InstrumentationMode instrumentationMode_ = InstrumentationMode::Full;
    uc_hook memoryHook_{};
    uc_hook memoryWriteHook_{};
    uc_hook codeHook_{};
    uc_hook blockHook_{};
    uc_hook getterReturnHook_{};
    uc_hook ringSvcHook_{};
    uc_hook hashEndHook_{};
    uc_hook invalidInstructionHook_{};
    uc_hook sysRegisterHook_{};
    uc_hook interruptHook_{};
    std::unordered_set<uint64_t> mappedAccessPages_;
    std::unordered_map<uint64_t, std::unique_ptr<std::array<uint8_t, kPageSize>>> remotePageBackings_;
    struct MemoryWrite
    {
        uint64_t accessAddress;
        uint64_t canonicalAddress;
        uint32_t size;
    };
    std::vector<MemoryWrite> memoryWrites_;
    uint32_t mappedRemotePages_ = 0;
    uint64_t executedInstructions_ = 0;
    uint64_t emulatedPacgaCount_ = 0;
    uint64_t emulatedSyscallCount_ = 0;
    uint64_t shellcodeIoctlCount_ = 0;
    uint64_t observedSeekFd_ = 0;
    uint64_t observedSeekResult_ = 0;
    uint64_t observedIoctlFd_ = 0;
    uint64_t unknownSyscallCount_ = 0;
    uint64_t lastUnknownSyscall_ = 0;
    uint64_t cntfrqReadCount_ = 0;
    uint64_t cntvctReadCount_ = 0;
    SyscallStage syscallStage_ = SyscallStage::Initial;
    uint64_t transformAddress_ = 0;
    uint64_t finalPc_ = 0;
    uint64_t finalX0Raw_ = 0;
    uint64_t finalTransformAddress_ = 0;
    uint64_t finalLr_ = 0;
    uint64_t transformWriteCount_ = 0;
    uint64_t retryAddress_ = 0;
    uint64_t lastCodeAddress_ = 0;
    uint32_t lastCodeInstruction_ = 0;
    uint32_t recoveredInstructions_ = 0;
    bool stoppedForError_ = false;
    bool retryPatchedInstruction_ = false;
    bool reachedGetterReturn_ = false;
    bool getterPageUnavailable_ = false;
    bool syscallContractViolation_ = false;
    bool hasObservedSeekFd_ = false;
    bool hasObservedIoctlFd_ = false;
    bool ringOwned_ = false;
    bool ringProofAccepted_ = false;
    uint64_t ringAddress_ = 0;
    uint64_t ringSlot_ = 0;
    uint64_t ringServiceHitCount_ = 0;
    uint64_t ringCaptureCount_ = 0;
    ResultMode resultMode_ = ResultMode::Passthrough;
    std::string failureReason_;
};
#endif
} // namespace

void ResetExecutionCaches()
{
    std::lock_guard<std::mutex> lock(gPagePlanMutex);
    gComponentPagePlans.clear();
}

#if defined(JIEMI_WITH_UNICORN)
void ExecuteDecryptTask(const DecryptTask &task)
{
    const auto taskStart = std::chrono::steady_clock::now();
    uint64_t queueWaitMicroseconds = 0;
    if (task.enqueuedAtNanoseconds != 0)
    {
        const uint64_t nowNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                taskStart.time_since_epoch())
                .count());
        if (nowNanoseconds > task.enqueuedAtNanoseconds)
            queueWaitMicroseconds =
                (nowNanoseconds - task.enqueuedAtNanoseconds) / 1000ULL;
    }

    uint64_t libUE4Base = 0;
    CoordDriverBridge::Environment environment{};
    RingServiceLayout ringLayout{};
    if (!GetTaskConfiguration(task, libUE4Base, environment, ringLayout))
        return;

    Coordinate coordinate{};
    Coordinate rawCoordinate{};
    ComponentPagePlan pagePlan{};
    const bool pagePlanHit = LoadComponentPagePlan(task, pagePlan);
    const bool rawCoordinateReady =
        !pagePlanHit &&
        CoordDriverBridge::Read(task.sceneComponent + 0x220,
                                &rawCoordinate, sizeof(rawCoordinate));

    std::unique_ptr<GetterExecution> discoveryExecution;
    RemoteSnapshot snapshot;
    std::unique_ptr<GetterExecution> replayExecution;
    std::unordered_set<uint64_t> sourcePages;
    uint32_t snapshotRound = 0;
    uint32_t snapshotChangedPages = 0;
    bool replaySucceeded = false;
    bool planRequiresFullInstrumentation =
        pagePlanHit && pagePlan.requiresFullInstrumentation;
    bool usedPreciseReplay = false;
    uint64_t observedSeekResult =
        pagePlanHit ? pagePlan.observedSeekResult : 0;
    uint64_t discoveryMicroseconds = 0;
    uint64_t snapshotMicroseconds = 0;
    uint64_t replayMicroseconds = 0;

    const auto captureSnapshot =
        [&](uint32_t readPasses)
    {
        const auto start = std::chrono::steady_clock::now();
        bool captured = false;
        {
            std::lock_guard<std::mutex> captureLock(gRemoteCaptureMutex);
            captured = CaptureRemoteSnapshot(
                sourcePages, snapshot, snapshotChangedPages, readPasses);
        }
        snapshotMicroseconds += ElapsedMicroseconds(start);
        return captured;
    };

    if (pagePlanHit)
    {
        sourcePages = pagePlan.sourcePages;
    }
    else
    {
        const auto discoveryStart = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> captureLock(gRemoteCaptureMutex);
            discoveryExecution = std::make_unique<GetterExecution>(
                libUE4Base + kGetterOffset, task.sceneComponent,
                environment, ringLayout);
            Coordinate ignoredDiscoveryCoordinate{};
            discoveryExecution->Run(ignoredDiscoveryCoordinate);
        }
        discoveryMicroseconds += ElapsedMicroseconds(discoveryStart);

        sourcePages = discoveryExecution->SourcePages();
        sourcePages.insert(
            CanonicalAddress(task.sceneComponent) & ~(kPageSize - 1));
        if (IsRemoteAddress(discoveryExecution->FinalX0Raw()))
            sourcePages.insert(
                CanonicalAddress(discoveryExecution->FinalX0Raw()) &
                ~(kPageSize - 1));
        observedSeekResult = discoveryExecution->ObservedSeekResult();
    }

    const uint32_t maximumReplayRounds =
        pagePlanHit ? kPlannedSnapshotReplayRounds : kSnapshotReplayRounds;
    const uint32_t snapshotReadPasses =
        pagePlanHit ? kPlannedSnapshotReadPasses : kSnapshotReadPasses;
    for (snapshotRound = 1; snapshotRound <= maximumReplayRounds; ++snapshotRound)
    {
        if (!captureSnapshot(snapshotReadPasses))
            break;

        const InstrumentationMode preferredMode =
            planRequiresFullInstrumentation ? InstrumentationMode::Full
                                            : (pagePlanHit
                                                   ? InstrumentationMode::Precise
                                                   : InstrumentationMode::Full);
        auto candidate = std::make_unique<GetterExecution>(
            libUE4Base + kGetterOffset, task.sceneComponent, environment, ringLayout,
            &snapshot, observedSeekResult, preferredMode);
        Coordinate replayCoordinate{};
        const auto replayStart = std::chrono::steady_clock::now();
        if (candidate->Run(replayCoordinate))
        {
            replayMicroseconds += ElapsedMicroseconds(replayStart);
            coordinate = replayCoordinate;
            replayExecution = std::move(candidate);
            replaySucceeded = true;
            usedPreciseReplay =
                preferredMode == InstrumentationMode::Precise;
            break;
        }
        replayMicroseconds += ElapsedMicroseconds(replayStart);

        uint64_t missingSnapshotPage = candidate->MissingSnapshotPage();
        replayExecution = std::move(candidate);
        if (missingSnapshotPage != 0)
        {
            sourcePages.insert(missingSnapshotPage);
            continue;
        }

        if (preferredMode == InstrumentationMode::Precise)
        {
            auto fallback = std::make_unique<GetterExecution>(
                libUE4Base + kGetterOffset, task.sceneComponent,
                environment, ringLayout, &snapshot, observedSeekResult,
                InstrumentationMode::Full);
            Coordinate fallbackCoordinate{};
            const auto fallbackStart = std::chrono::steady_clock::now();
            if (fallback->Run(fallbackCoordinate))
            {
                replayMicroseconds += ElapsedMicroseconds(fallbackStart);
                coordinate = fallbackCoordinate;
                replayExecution = std::move(fallback);
                replaySucceeded = true;
                planRequiresFullInstrumentation = true;
                usedPreciseReplay = false;
                break;
            }
            replayMicroseconds += ElapsedMicroseconds(fallbackStart);
            missingSnapshotPage = fallback->MissingSnapshotPage();
            replayExecution = std::move(fallback);
            if (missingSnapshotPage != 0)
            {
                sourcePages.insert(missingSnapshotPage);
                planRequiresFullInstrumentation = true;
                continue;
            }
        }
        if (missingSnapshotPage == 0)
            break;
    }

    if (!replayExecution && !discoveryExecution)
    {
        InvalidateComponentPagePlan(task);
        StoreTaskFailure(task, false);
        SetStatus("unable to capture a fixed remote snapshot");
        return;
    }

    GetterExecution &execution =
        replayExecution ? *replayExecution : *discoveryExecution;
    if (!replaySucceeded)
    {
        if (pagePlanHit)
            InvalidateComponentPagePlan(task);
        char seekFd[32] = "none";
        char ioctlFd[32] = "none";
        if (execution.HasObservedSeekFd())
            std::snprintf(seekFd, sizeof(seekFd), "0x%llX",
                          static_cast<unsigned long long>(execution.ObservedSeekFd()));
        if (execution.HasObservedIoctlFd())
            std::snprintf(ioctlFd, sizeof(ioctlFd), "0x%llX",
                          static_cast<unsigned long long>(execution.ObservedIoctlFd()));
        char diagnostic[1200]{};
        std::snprintf(
            diagnostic, sizeof(diagnostic),
            "failure component=0x%llX reason=%s final_pc=0x%llX final_lr=0x%llX final_x0_raw=0x%llX ring=0x%llX ring_slot=%llu ring_owned=%s ring_hits=%llu ring_captures=%llu insns=%llu pages=%u tid=%llu pacga=%llu syscalls=%llu syscall_contract=%s seek_fd=%s seek_result=0x%llX ioctl_fd=%s shellcode_ioctls=%llu unknown_syscalls=%llu last_unknown=%llu cntfrq_reads=%llu cntvct_reads=%llu snapshot_round=%u snapshot_pages=%zu snapshot_changed=%u recovered=%u plan=%s queue_us=%llu discovery_us=%llu snapshot_us=%llu replay_us=%llu total_us=%llu",
            static_cast<unsigned long long>(task.sceneComponent),
            execution.FailureReason().c_str(),
            static_cast<unsigned long long>(execution.FinalProgramCounter()),
            static_cast<unsigned long long>(execution.FinalLinkRegister()),
            static_cast<unsigned long long>(execution.FinalX0Raw()),
            static_cast<unsigned long long>(execution.RingAddress()),
            static_cast<unsigned long long>(execution.RingSlot()),
            execution.RingOwned() ? "yes" : "no",
            static_cast<unsigned long long>(execution.RingServiceHitCount()),
            static_cast<unsigned long long>(execution.RingCaptureCount()),
            static_cast<unsigned long long>(execution.ExecutedInstructions()),
            execution.MappedRemotePages(),
            static_cast<unsigned long long>(environment.tid),
            static_cast<unsigned long long>(execution.EmulatedPacgaCount()),
            static_cast<unsigned long long>(execution.EmulatedSyscallCount()),
            execution.SyscallContractName(),
            seekFd,
            static_cast<unsigned long long>(execution.ObservedSeekResult()),
            ioctlFd,
            static_cast<unsigned long long>(execution.ShellcodeIoctlCount()),
            static_cast<unsigned long long>(execution.UnknownSyscallCount()),
            static_cast<unsigned long long>(execution.LastUnknownSyscall()),
            static_cast<unsigned long long>(execution.CntfrqReadCount()),
            static_cast<unsigned long long>(execution.CntvctReadCount()),
            std::min(snapshotRound, kSnapshotReplayRounds),
            snapshot.size(),
            snapshotChangedPages,
            execution.RecoveredInstructions(),
            pagePlanHit ? "hit" : "miss",
            static_cast<unsigned long long>(queueWaitMicroseconds),
            static_cast<unsigned long long>(discoveryMicroseconds),
            static_cast<unsigned long long>(snapshotMicroseconds),
            static_cast<unsigned long long>(replayMicroseconds),
            static_cast<unsigned long long>(ElapsedMicroseconds(taskStart)));
        LogDiagnosticThrottled(diagnostic);
        StoreTaskFailure(task, execution.GetterPageUnavailable());
        SetStatus(execution.FailureReason());
        return;
    }

    std::unordered_set<uint64_t> validatedPages = execution.SourcePages();
    validatedPages.insert(
        CanonicalAddress(task.sceneComponent) & ~(kPageSize - 1));
    if (IsRemoteAddress(execution.FinalX0Raw()))
        validatedPages.insert(
            CanonicalAddress(execution.FinalX0Raw()) & ~(kPageSize - 1));
    StoreComponentPagePlan(task, std::move(validatedPages),
                           execution.ObservedSeekResult(),
                           planRequiresFullInstrumentation);

    if (!StoreTaskCoordinate(task, coordinate))
        return;

    char seekFd[32] = "none";
    char ioctlFd[32] = "none";
    if (execution.HasObservedSeekFd())
        std::snprintf(seekFd, sizeof(seekFd), "0x%llX",
                      static_cast<unsigned long long>(execution.ObservedSeekFd()));
    if (execution.HasObservedIoctlFd())
        std::snprintf(ioctlFd, sizeof(ioctlFd), "0x%llX",
                      static_cast<unsigned long long>(execution.ObservedIoctlFd()));
    char diagnostic[1280]{};
    std::snprintf(diagnostic, sizeof(diagnostic),
                  "coordinate component=0x%llX raw220=%s(%.3f, %.3f, %.3f) final_pc=0x%llX final_lr=0x%llX final_x0_raw=0x%llX result_read=0x%llX result_mode=%s result=(%.3f, %.3f, %.3f) same=%s transform_writes=%llu ring=0x%llX ring_slot=%llu ring_owned=%s ring_hits=%llu ring_captures=%llu insns=%llu pages=%u tid=%llu pacga=%llu syscalls=%llu syscall_contract=%s seek_fd=%s seek_result=0x%llX ioctl_fd=%s shellcode_ioctls=%llu unknown_syscalls=%llu last_unknown=%llu cntfrq_reads=%llu cntvct_reads=%llu snapshot_round=%u snapshot_pages=%zu snapshot_changed=%u recovered=%u plan=%s precise=%s queue_us=%llu discovery_us=%llu snapshot_us=%llu replay_us=%llu total_us=%llu",
                  static_cast<unsigned long long>(task.sceneComponent),
                  rawCoordinateReady ? "" : "unavailable ",
                  rawCoordinate.x, rawCoordinate.y, rawCoordinate.z,
                  static_cast<unsigned long long>(execution.FinalProgramCounter()),
                  static_cast<unsigned long long>(execution.FinalLinkRegister()),
                  static_cast<unsigned long long>(execution.FinalX0Raw()),
                  static_cast<unsigned long long>(execution.TransformAddress()),
                  execution.ResultModeName(),
                  coordinate.x, coordinate.y, coordinate.z,
                  rawCoordinateReady && std::memcmp(&rawCoordinate, &coordinate, sizeof(coordinate)) == 0 ? "yes" : "no",
                  static_cast<unsigned long long>(execution.TransformWriteCount()),
                  static_cast<unsigned long long>(execution.RingAddress()),
                  static_cast<unsigned long long>(execution.RingSlot()),
                  execution.RingOwned() ? "yes" : "no",
                  static_cast<unsigned long long>(execution.RingServiceHitCount()),
                  static_cast<unsigned long long>(execution.RingCaptureCount()),
                  static_cast<unsigned long long>(execution.ExecutedInstructions()),
                  execution.MappedRemotePages(),
                  static_cast<unsigned long long>(environment.tid),
                  static_cast<unsigned long long>(execution.EmulatedPacgaCount()),
                  static_cast<unsigned long long>(execution.EmulatedSyscallCount()),
                  execution.SyscallContractName(),
                  seekFd,
                  static_cast<unsigned long long>(execution.ObservedSeekResult()),
                  ioctlFd,
                  static_cast<unsigned long long>(execution.ShellcodeIoctlCount()),
                  static_cast<unsigned long long>(execution.UnknownSyscallCount()),
                  static_cast<unsigned long long>(execution.LastUnknownSyscall()),
                  static_cast<unsigned long long>(execution.CntfrqReadCount()),
                  static_cast<unsigned long long>(execution.CntvctReadCount()),
                  snapshotRound,
                  snapshot.size(),
                  snapshotChangedPages,
                  execution.RecoveredInstructions(),
                  pagePlanHit ? "hit" : "miss",
                  usedPreciseReplay ? "yes" : "no",
                  static_cast<unsigned long long>(queueWaitMicroseconds),
                  static_cast<unsigned long long>(discoveryMicroseconds),
                  static_cast<unsigned long long>(snapshotMicroseconds),
                  static_cast<unsigned long long>(replayMicroseconds),
                  static_cast<unsigned long long>(ElapsedMicroseconds(taskStart)));
    LogDiagnosticThrottled(diagnostic);
    SetStatus("getter emulation returned a coordinate");
}
#endif

} // namespace CoordDecrypt::detail
