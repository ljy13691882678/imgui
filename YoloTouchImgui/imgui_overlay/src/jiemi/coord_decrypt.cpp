#include "jiemi/coord_decrypt_internal.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <sys/stat.h>

namespace CoordDecrypt
{
namespace detail
{
namespace
{
#if defined(COORD_DECRYPT_LOG_DUCK)
#define COORD_DECRYPT_LOG_PATH "/sdcard/duck_coord_decrypt.log"
#elif defined(COORD_DECRYPT_LOG_AG)
#define COORD_DECRYPT_LOG_PATH "/sdcard/ag_coord_decrypt.log"
#elif defined(COORD_DECRYPT_LOG_LEGION)
#define COORD_DECRYPT_LOG_PATH "/sdcard/legion_coord_decrypt.log"
#elif !defined(COORD_DECRYPT_LOG_PATH)
#define COORD_DECRYPT_LOG_PATH "/sdcard/coord_decrypt.log"
#endif
constexpr const char *kLogPath = COORD_DECRYPT_LOG_PATH;
constexpr long kMaximumLogSize = 2 * 1024 * 1024;
constexpr std::chrono::milliseconds kEnvironmentRefreshInterval{5000};
constexpr std::chrono::milliseconds kCoordinateCacheLifetime{33};
constexpr std::chrono::milliseconds kCoordinateMaximumStaleLifetime{250};
constexpr std::chrono::milliseconds kFailureCooldown{100};
constexpr std::chrono::milliseconds kGlobalFailureCooldown{500};
constexpr std::chrono::milliseconds kMaximumTaskQueueAge{100};
constexpr std::chrono::seconds kDiagnosticLogInterval{1};
constexpr size_t kMaximumPendingDecryptTasks = 512;
constexpr size_t kDecryptWorkerCount = 6;

uint64_t SteadyNowNanoseconds()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

bool SameEnvironment(const CoordDriverBridge::Environment &left,
                     const CoordDriverBridge::Environment &right)
{
    return left.processId == right.processId && left.tls == right.tls &&
           left.pacgaLo == right.pacgaLo &&
           left.pacgaHi == right.pacgaHi && left.tid == right.tid;
}
struct CachedCoordinate
{
    Coordinate coordinate{};
    std::chrono::steady_clock::time_point refreshAt{};
    std::chrono::steady_clock::time_point validUntil{};
};

struct SessionState
{
    std::mutex mutex;
    std::condition_variable workerWake;
    std::array<std::thread, kDecryptWorkerCount> workers;
    bool workerStop = false;
    bool enabled = false;
    uint64_t generation = 0;
    uint64_t environmentEpoch = 0;
    uint64_t libUE4Base = 0;
    CoordDriverBridge::Environment environment{};
    RingServiceLayout ringLayout{};
    bool environmentReady = false;
    bool ringLayoutReady = false;
    bool environmentRefreshRequested = false;
    bool environmentRefreshInProgress = false;
    std::chrono::steady_clock::time_point nextEnvironmentRefresh{};
    std::chrono::steady_clock::time_point executionBlockedUntil{};
    std::chrono::steady_clock::time_point nextDiagnosticLog{};
    std::deque<DecryptTask> tasks;
    std::unordered_map<uint64_t, uint64_t> pendingComponents;
    size_t activeDecryptTasks = 0;
    std::unordered_map<uint64_t, CachedCoordinate> coordinates;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> failedUntil;
    std::string lastStatus;
};

SessionState g_session;
std::mutex g_logMutex;
std::mutex g_workerLifecycleMutex;

void AppendFileLog(const std::string &status)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    const char *mode = "a";
    struct stat fileStatus{};
    if (stat(kLogPath, &fileStatus) == 0 && fileStatus.st_size >= kMaximumLogSize)
        mode = "w";

    FILE *file = std::fopen(kLogPath, mode);
    if (!file)
        return;

    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&now, &localTime);
    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);
    std::fprintf(file, "[%s] [CoordDecrypt] %s\n", timestamp, status.c_str());
    std::fclose(file);
}

void SetStatus(std::string status)
{
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (status == g_session.lastStatus)
            return;
        g_session.lastStatus = status;
    }
    std::printf("[CoordDecrypt] %s\n", status.c_str());
    std::fflush(stdout);
    AppendFileLog(status);
}

void LogDiagnosticThrottled(const std::string &message)
{
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (now < g_session.nextDiagnosticLog)
            return;
        g_session.nextDiagnosticLog = now + kDiagnosticLogInterval;
    }
    std::printf("[CoordDecrypt] %s\n", message.c_str());
    std::fflush(stdout);
    AppendFileLog(message);
}
enum class CoordinateCacheState
{
    Missing,
    Fresh,
    Stale,
};

CoordinateCacheState GetCachedCoordinate(uint64_t sceneComponent,
                                         Coordinate &coordinate)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_session.mutex);
    const auto found = g_session.coordinates.find(sceneComponent);
    if (found == g_session.coordinates.end())
        return CoordinateCacheState::Missing;
    if (found->second.validUntil < now)
    {
        g_session.coordinates.erase(found);
        return CoordinateCacheState::Missing;
    }
    coordinate = found->second.coordinate;
    return found->second.refreshAt > now ? CoordinateCacheState::Fresh : CoordinateCacheState::Stale;
}

bool QueueDecryptTask(uint64_t sceneComponent)
{
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (!g_session.enabled || !g_session.environmentReady || g_session.libUE4Base == 0 ||
            std::chrono::steady_clock::now() < g_session.executionBlockedUntil)
            return false;

        const auto failed = g_session.failedUntil.find(sceneComponent);
        if (failed != g_session.failedUntil.end())
        {
            if (failed->second > now)
                return false;
            g_session.failedUntil.erase(failed);
        }

        if (g_session.pendingComponents.contains(sceneComponent) ||
            g_session.tasks.size() >= kMaximumPendingDecryptTasks)
            return false;

        const uint64_t generation = g_session.generation;
        g_session.tasks.push_back(
            {sceneComponent, generation, g_session.environmentEpoch, SteadyNowNanoseconds()});
        g_session.pendingComponents[sceneComponent] = generation;
    }
    g_session.workerWake.notify_one();
    return true;
}

void CompleteDecryptTaskLocked(const DecryptTask &task)
{
    const auto pending = g_session.pendingComponents.find(task.sceneComponent);
    if (pending != g_session.pendingComponents.end() && pending->second == task.generation)
        g_session.pendingComponents.erase(pending);
}

bool GetTaskConfiguration(const DecryptTask &task, uint64_t &libUE4Base,
                          CoordDriverBridge::Environment &environment,
                          RingServiceLayout &ringLayout)
{
    std::lock_guard<std::mutex> lock(g_session.mutex);
    const uint64_t nowNanoseconds = SteadyNowNanoseconds();
    const uint64_t maximumQueueAgeNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(kMaximumTaskQueueAge).count());
    const bool taskExpired =
        task.enqueuedAtNanoseconds != 0 && nowNanoseconds > task.enqueuedAtNanoseconds &&
        nowNanoseconds - task.enqueuedAtNanoseconds > maximumQueueAgeNanoseconds;
    if (g_session.workerStop || !g_session.enabled || task.generation != g_session.generation ||
        task.environmentEpoch != g_session.environmentEpoch ||
        !g_session.environmentReady || !g_session.ringLayoutReady ||
        g_session.libUE4Base == 0 || taskExpired ||
        std::chrono::steady_clock::now() < g_session.executionBlockedUntil)
    {
        CompleteDecryptTaskLocked(task);
        return false;
    }
    libUE4Base = g_session.libUE4Base;
    environment = g_session.environment;
    ringLayout = g_session.ringLayout;
    return true;
}

void StoreTaskFailure(const DecryptTask &task, bool getterPageUnavailable)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_session.mutex);
    CompleteDecryptTaskLocked(task);
    if (!g_session.enabled || task.generation != g_session.generation ||
        task.environmentEpoch != g_session.environmentEpoch)
        return;
    if (g_session.failedUntil.size() > 512)
        g_session.failedUntil.clear();
    g_session.failedUntil[task.sceneComponent] = now + kFailureCooldown;
    if (getterPageUnavailable)
        g_session.executionBlockedUntil = now + kGlobalFailureCooldown;
}

bool StoreTaskCoordinate(const DecryptTask &task, const Coordinate &coordinate)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_session.mutex);
    CompleteDecryptTaskLocked(task);
    if (!g_session.enabled || task.generation != g_session.generation ||
        task.environmentEpoch != g_session.environmentEpoch)
        return false;
    if (g_session.coordinates.size() > 512)
        g_session.coordinates.clear();
    g_session.coordinates[task.sceneComponent] = {
        coordinate,
        now + kCoordinateCacheLifetime,
        now + kCoordinateMaximumStaleLifetime,
    };
    g_session.failedUntil.erase(task.sceneComponent);
    return true;
}
bool RefreshEnvironmentOnWorker()
{
    uint64_t generation = 0;
    uint64_t libUE4Base = 0;
    int previousProcessId = 0;
    bool cachedRingLayoutReady = false;
    RingServiceLayout cachedRingLayout{};
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (!g_session.enabled || !g_session.environmentRefreshRequested ||
            g_session.environmentRefreshInProgress || g_session.libUE4Base == 0)
            return false;
        generation = g_session.generation;
        libUE4Base = g_session.libUE4Base;
        previousProcessId = g_session.environment.processId;
        cachedRingLayoutReady = g_session.ringLayoutReady;
        cachedRingLayout = g_session.ringLayout;
        g_session.environmentRefreshRequested = false;
        g_session.environmentRefreshInProgress = true;
    }

    CoordDriverBridge::Environment environment{};
    const bool environmentReady = CoordDriverBridge::GetGameThreadEnvironment(environment);
    RingServiceLayout ringLayout = cachedRingLayout;
    std::string ringLayoutError;
    bool ringLayoutReady = cachedRingLayoutReady;
    if (environmentReady &&
        (!cachedRingLayoutReady || environment.processId != previousProcessId))
    {
        ringLayoutReady = DiscoverRingServiceLayout(
            libUE4Base + kGetterOffset, ringLayout, ringLayoutError);
    }
    bool accepted = false;
    bool resetExecutionCaches = false;
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (g_session.enabled && generation == g_session.generation &&
            libUE4Base == g_session.libUE4Base)
        {
            const bool environmentChanged =
                g_session.environmentReady != environmentReady ||
                (environmentReady && !SameEnvironment(g_session.environment, environment));
            if (environmentChanged)
            {
                ++g_session.environmentEpoch;
                g_session.tasks.clear();
                g_session.pendingComponents.clear();
                g_session.coordinates.clear();
                g_session.failedUntil.clear();
                resetExecutionCaches = true;
            }
            g_session.environmentReady = environmentReady;
            g_session.environment = environmentReady ? environment : CoordDriverBridge::Environment{};
            g_session.ringLayoutReady = environmentReady && ringLayoutReady;
            g_session.ringLayout =
                g_session.ringLayoutReady ? ringLayout : RingServiceLayout{};
            g_session.nextEnvironmentRefresh =
                std::chrono::steady_clock::now() + kEnvironmentRefreshInterval;
            accepted = true;
        }
        g_session.environmentRefreshInProgress = false;
    }
    if (resetExecutionCaches)
        ResetExecutionCaches();
    g_session.workerWake.notify_all();
    if (accepted && !environmentReady)
        SetStatus("GameThread TLS/APGA/TID environment is unavailable");
    else if (accepted && !ringLayoutReady)
        SetStatus("ring-service Capstone discovery failed: " + ringLayoutError);
    else if (accepted && ringLayoutReady && !cachedRingLayoutReady)
    {
        char status[640]{};
        std::snprintf(
            status, sizeof(status),
            "ring-service Capstone ready shellcode=0x%llX entry=0x%llX region=[0x%llX,+0x%llX] svc=0x%llX hash_end=0x%llX hash_reg=X%u ring_calc=0x%llX ring_offset=0x%llX stride=0x%llX dispatcher=0x%llX state_offset=0x%X state_guards=%zu legacy_guards=%zu",
            static_cast<unsigned long long>(ringLayout.shellcode),
            static_cast<unsigned long long>(ringLayout.entry),
            static_cast<unsigned long long>(ringLayout.regionBase),
            static_cast<unsigned long long>(ringLayout.regionSize),
            static_cast<unsigned long long>(ringLayout.svc),
            static_cast<unsigned long long>(ringLayout.hashEnd),
            ringLayout.hashRegister,
            static_cast<unsigned long long>(ringLayout.ringCalc),
            static_cast<unsigned long long>(ringLayout.ringOffset),
            static_cast<unsigned long long>(ringLayout.ringStride),
            static_cast<unsigned long long>(ringLayout.dispatcher),
            ringLayout.stateSlotOffset,
            ringLayout.stateGuards.size(),
            ringLayout.legacyGuards.size());
        SetStatus(status);
    }
    return true;
}

void DecryptWorkerMain()
{
    for (;;)
    {
        DecryptTask task{};
        {
            std::unique_lock<std::mutex> lock(g_session.mutex);
            g_session.workerWake.wait(lock, []
                                      {
                                          return g_session.workerStop ||
                                                 (!g_session.environmentRefreshInProgress &&
                                                  ((g_session.environmentRefreshRequested &&
                                                    g_session.activeDecryptTasks == 0) ||
                                                   (!g_session.environmentRefreshRequested &&
                                                    !g_session.tasks.empty())));
                                      });
            if (g_session.workerStop)
                return;
            if (g_session.environmentRefreshRequested)
            {
                lock.unlock();
                RefreshEnvironmentOnWorker();
                continue;
            }
            task = g_session.tasks.front();
            g_session.tasks.pop_front();
            ++g_session.activeDecryptTasks;
        }

#if defined(JIEMI_WITH_UNICORN)
        ExecuteDecryptTask(task);
#else
        {
            std::lock_guard<std::mutex> lock(g_session.mutex);
            CompleteDecryptTaskLocked(task);
        }
#endif
        bool wakeEnvironmentRefresh = false;
        {
            std::lock_guard<std::mutex> lock(g_session.mutex);
            if (g_session.activeDecryptTasks > 0)
                --g_session.activeDecryptTasks;
            wakeEnvironmentRefresh =
                g_session.environmentRefreshRequested &&
                g_session.activeDecryptTasks == 0;
        }
        if (wakeEnvironmentRefresh)
            g_session.workerWake.notify_all();
    }
}

void EnsureDecryptWorkerStarted()
{
    std::lock_guard<std::mutex> lifecycleLock(g_workerLifecycleMutex);
    std::lock_guard<std::mutex> lock(g_session.mutex);
    for (const std::thread &worker : g_session.workers)
        if (worker.joinable())
            return;
    g_session.workerStop = false;
    for (std::thread &worker : g_session.workers)
        worker = std::thread(DecryptWorkerMain);
}
} // namespace detail

using namespace detail;

void Tick(bool enabled, uint64_t libUE4Base)
{
    if (!enabled || libUE4Base == 0)
    {
        Reset();
        return;
    }

    EnsureDecryptWorkerStarted();
    const auto now = std::chrono::steady_clock::now();
    bool wakeWorker = false;
    bool resetExecutionCaches = false;
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (!g_session.enabled || g_session.libUE4Base != libUE4Base)
        {
            ++g_session.generation;
            ++g_session.environmentEpoch;
            g_session.enabled = true;
            g_session.libUE4Base = libUE4Base;
            g_session.environmentReady = false;
            g_session.ringLayoutReady = false;
            g_session.ringLayout = {};
            g_session.environmentRefreshRequested = true;
            g_session.environmentRefreshInProgress = false;
            g_session.nextEnvironmentRefresh = {};
            g_session.executionBlockedUntil = {};
            g_session.tasks.clear();
            g_session.pendingComponents.clear();
            g_session.coordinates.clear();
            g_session.failedUntil.clear();
            wakeWorker = true;
            resetExecutionCaches = true;
        }
        else if (!g_session.environmentRefreshRequested && !g_session.environmentRefreshInProgress &&
                 (!g_session.environmentReady || now >= g_session.nextEnvironmentRefresh))
        {
            g_session.environmentRefreshRequested = true;
            wakeWorker = true;
        }
    }
    if (resetExecutionCaches)
        ResetExecutionCaches();
    if (wakeWorker)
        g_session.workerWake.notify_one();
}

void Reset()
{
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        if (!g_session.enabled && g_session.libUE4Base == 0)
            return;
        ++g_session.generation;
        ++g_session.environmentEpoch;
        g_session.enabled = false;
        g_session.libUE4Base = 0;
        g_session.environment = {};
        g_session.environmentReady = false;
        g_session.ringLayoutReady = false;
        g_session.ringLayout = {};
        g_session.environmentRefreshRequested = false;
        g_session.nextEnvironmentRefresh = {};
        g_session.executionBlockedUntil = {};
        g_session.nextDiagnosticLog = {};
        g_session.tasks.clear();
        g_session.pendingComponents.clear();
        g_session.coordinates.clear();
        g_session.failedUntil.clear();
        g_session.lastStatus.clear();
    }
    ResetExecutionCaches();
}

void Shutdown()
{
    std::lock_guard<std::mutex> lifecycleLock(g_workerLifecycleMutex);
    std::array<std::thread, kDecryptWorkerCount> workers;
    {
        std::lock_guard<std::mutex> lock(g_session.mutex);
        ++g_session.generation;
        ++g_session.environmentEpoch;
        g_session.enabled = false;
        g_session.libUE4Base = 0;
        g_session.environmentReady = false;
        g_session.ringLayoutReady = false;
        g_session.ringLayout = {};
        g_session.environmentRefreshRequested = false;
        g_session.tasks.clear();
        g_session.pendingComponents.clear();
        g_session.coordinates.clear();
        g_session.workerStop = true;
        workers = std::move(g_session.workers);
    }
    g_session.workerWake.notify_all();
    for (std::thread &worker : workers)
        if (worker.joinable())
            worker.join();
    ResetExecutionCaches();
}

bool TryRead(uint64_t sceneComponent, Coordinate &coordinate)
{
    coordinate = {};
    if (!sceneComponent)
        return false;

#if !defined(JIEMI_WITH_UNICORN)
    SetStatus("Unicorn static library is unavailable in this build");
    return false;
#else
    const CoordinateCacheState cacheState =
        GetCachedCoordinate(sceneComponent, coordinate);
    if (cacheState != CoordinateCacheState::Fresh)
        QueueDecryptTask(sceneComponent);
    return cacheState != CoordinateCacheState::Missing;
#endif
}

bool TryReadSmoothed(uint64_t sceneComponent, Coordinate &coordinate)
{
    coordinate = {};
    if (!sceneComponent)
        return false;

#if !defined(JIEMI_WITH_UNICORN)
    SetStatus("Unicorn static library is unavailable in this build");
    return false;
#else
    const CoordinateCacheState cacheState =
        GetCachedCoordinate(sceneComponent, coordinate);
    if (cacheState != CoordinateCacheState::Fresh)
        QueueDecryptTask(sceneComponent);
    return cacheState != CoordinateCacheState::Missing;
#endif
}

} // namespace CoordDecrypt
