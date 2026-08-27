// coord_driver_bridge.cpp — 基于 TimeDriver 实现 CoordDriverBridge
// 为坐标解密（CoordDecrypt）提供内存读取、可读内存区间、GameThread 环境与文件大小能力。
// 移植自 udp_decode 参考工程 src/main.cpp 中的 CoordDriverBridge 实现，
// 底层驱动由 TimeDriver(内核读写) 提供。所有读写都遵循“驱动已连接才允许”的防机制。
#include "jiemi/coord_driver_bridge.h"

#include "time_driver.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace CoordDriverBridge
{
namespace
{

constexpr const char *kTargetPackage = "com.tencent.tmgp.dfm";
constexpr std::chrono::seconds kPidCacheLifetime{2};

// 通过 /proc 按包名查 PID（与 memory_esp 一致）
pid_t FindPidByCmdline(const char *pkg)
{
    if (!pkg || !*pkg)
        return 0;
    DIR *dir = opendir("/proc");
    if (!dir)
        return 0;
    struct dirent *ent;
    char path[64], line[512];
    pid_t found = 0;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;
        const int fd = snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
        (void)fd;
        const int fd2 = open(path, O_RDONLY);
        if (fd2 < 0)
            continue;
        ssize_t n = read(fd2, line, sizeof(line) - 1);
        close(fd2);
        if (n <= 0)
            continue;
        line[n] = 0;
        if (strstr(line, pkg))
        {
            found = (pid_t)atoi(ent->d_name);
            break;
        }
    }
    closedir(dir);
    return found;
}

// 带缓存的游戏 PID：每次搜索都遍历 /proc 开销较大，读透视主循环每帧会调用，
// 因此缓存 N 秒再重新解析，兼顾性能与进程重启后的更新。
pid_t GetGamePid()
{
    static pid_t cached = 0;
    static std::chrono::steady_clock::time_point lastCheck{};
    const auto now = std::chrono::steady_clock::now();
    if (cached > 0 && now - lastCheck < kPidCacheLifetime)
        return cached;
    const pid_t found = FindPidByCmdline(kTargetPackage);
    cached = found;
    lastCheck = now;
    return cached;
}

bool IsThreadName(int processId, uint64_t threadId, const char *threadName)
{
    if (processId <= 0 || threadId == 0 || !threadName || !*threadName)
        return false;
    char commPath[96]{};
    snprintf(commPath, sizeof(commPath), "/proc/%d/task/%llu/comm", processId,
             static_cast<unsigned long long>(threadId));
    FILE *comm = std::fopen(commPath, "r");
    if (!comm)
        return false;
    char currentName[64]{};
    const bool read = std::fgets(currentName, sizeof(currentName), comm) != nullptr;
    std::fclose(comm);
    if (!read)
        return false;
    currentName[strcspn(currentName, "\r\n")] = '\0';
    return strcmp(currentName, threadName) == 0;
}

// 枚举进程内的所有线程 id
bool ListProcessThreads(int processId, std::vector<uint64_t> &out)
{
    out.clear();
    if (processId <= 0)
        return false;
    char taskPath[64]{};
    snprintf(taskPath, sizeof(taskPath), "/proc/%d/task", processId);
    DIR *dir = opendir(taskPath);
    if (!dir)
        return false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;
        const unsigned long long tid = strtoull(ent->d_name, nullptr, 10);
        if (tid != 0)
            out.push_back((uint64_t)tid);
    }
    closedir(dir);
    return !out.empty();
}

} // namespace

bool Read(uint64_t address, void *buffer, size_t size)
{
    if (!TIME_Driver || !TIME_Driver->IsConnected() || !buffer || size == 0)
        return false;
    const pid_t pid = GetGamePid();
    if (pid <= 0)
        return false;
    return TIME_Driver->Read_Memory_Fast(pid, address, buffer, size);
}

std::vector<std::pair<uintptr_t, uintptr_t>> GetScanRegions()
{
    std::vector<std::pair<uintptr_t, uintptr_t>> regions;
    const pid_t pid = GetGamePid();
    if (pid <= 0 || !TIME_Driver || !TIME_Driver->IsConnected())
        return regions;

    char mapsPath[64]{};
    snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
    FILE *maps = std::fopen(mapsPath, "r");
    if (!maps)
        return regions;

    char line[1024]{};
    while (std::fgets(line, sizeof(line), maps))
    {
        unsigned long long start = 0, end = 0;
        char perms[8] = {};
        // 格式: start-end perms offset dev inode pathname
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3)
            continue;
        // 仅返回可读区间（解密需要读入页面做反汇编/模拟执行）
        if (perms[0] != 'r')
            continue;
        if (end <= start)
            continue;
        regions.emplace_back((uintptr_t)start, (uintptr_t)end);
    }
    std::fclose(maps);
    return regions;
}

bool GetFileDescriptorSize(int processId, uint64_t fileDescriptor, uint64_t &size)
{
    size = 0;
    if (processId <= 0 || fileDescriptor > static_cast<uint64_t>(INT_MAX))
        return false;

    const int pidFd = static_cast<int>(syscall(SYS_pidfd_open, processId, 0));
    if (pidFd < 0)
        return false;

    const int duplicateFd =
        static_cast<int>(syscall(SYS_pidfd_getfd, pidFd, static_cast<int>(fileDescriptor), 0));
    if (duplicateFd < 0)
    {
        close(pidFd);
        return false;
    }

    struct stat fileStatus{};
    const bool ok = fstat(duplicateFd, &fileStatus) == 0 && fileStatus.st_size >= 0;
    if (ok)
        size = static_cast<uint64_t>(fileStatus.st_size);
    close(duplicateFd);
    close(pidFd);
    return ok;
}

bool GetGameThreadEnvironment(Environment &environment)
{
    environment = {};
    if (!TIME_Driver || !TIME_Driver->IsConnected())
        return false;

    const int processId = GetGamePid();
    if (processId <= 0)
        return false;

    // 找到 GameThread 线程，读取其 tpidr_el0(tls) 与 PACGA 密钥
    std::vector<uint64_t> threads;
    if (!ListProcessThreads(processId, threads))
        return false;

    bool foundThread = false;
    uint64_t tid = 0;
    uint64_t tls = 0;
    uint64_t pacgaLo = 0, pacgaHi = 0;
    for (uint64_t candidate : threads)
    {
        if (!IsThreadName(processId, candidate, "GameThread"))
            continue;
        const uint64_t candidateTls = TIME_Driver->Get_Thread_Tpidr_El0((pid_t)candidate);
        uint64_t lo = 0, hi = 0;
        const bool pacgaOk = TIME_Driver->Get_Pacga_Key((pid_t)candidate, &lo, &hi);
        if (candidateTls != 0 && pacgaOk)
        {
            tid = candidate;
            tls = candidateTls;
            pacgaLo = lo;
            pacgaHi = hi;
            foundThread = true;
        }
        break; // 匹配到 GameThread 即可
    }
    if (!foundThread)
        return false;

    environment.processId = processId;
    environment.tls = tls;
    environment.pacgaLo = pacgaLo;
    environment.pacgaHi = pacgaHi;
    environment.tid = tid;
    return true;
}

} // namespace CoordDriverBridge