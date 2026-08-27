#pragma once

#include "jiemi/coord_decrypt.h"
#include "jiemi/coord_driver_bridge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace CoordDecrypt::detail
{
inline constexpr uint64_t kGetterOffset = 0x0E738950ULL;
inline constexpr uint64_t kPageSize = 0x1000ULL;
inline constexpr uint64_t kAddressMask = 0x00FFFFFFFFFFFFFFULL;
inline constexpr uint64_t kMaximumUserAddress = 0x0000FFFFFFFFFFFFULL;

struct RingStateGuard
{
    uint32_t componentStackOffset = 0;
    uint32_t selector = 0;
};

struct RingLegacyGuard
{
    uint32_t componentStackOffset = 0;
    uint64_t entry = 0;
};

struct RingServiceLayout
{
    bool valid = false;
    uint64_t shellcode = 0;
    uint64_t entry = 0;
    uint64_t regionBase = 0;
    uint64_t regionSize = 0;
    uint64_t svc = 0;
    uint64_t hashEnd = 0;
    uint32_t hashRegister = 0;
    uint64_t ringCalc = 0;
    uint64_t ringOffset = 0;
    uint64_t ringStride = 0;
    uint64_t dispatcher = 0;
    uint32_t stateSlotOffset = 0;
    uint32_t stateSelector = 0;
    std::vector<RingStateGuard> stateGuards;
    std::vector<RingLegacyGuard> legacyGuards;
};

struct DecryptTask
{
    uint64_t sceneComponent = 0;
    uint64_t generation = 0;
    uint64_t environmentEpoch = 0;
    uint64_t enqueuedAtNanoseconds = 0;
};

inline uint64_t CanonicalAddress(uint64_t address)
{
    return address & kAddressMask;
}

inline bool IsRemoteAddress(uint64_t address)
{
    address = CanonicalAddress(address);
    return address >= 0x10000000ULL && address <= kMaximumUserAddress;
}

bool DiscoverRingServiceLayout(uint64_t getterAddress, RingServiceLayout &layout,
                               std::string &error);

void SetStatus(std::string status);
void LogDiagnosticThrottled(const std::string &message);
bool GetTaskConfiguration(const DecryptTask &task, uint64_t &libUE4Base,
                          CoordDriverBridge::Environment &environment,
                          RingServiceLayout &ringLayout);
void StoreTaskFailure(const DecryptTask &task, bool getterPageUnavailable);
bool StoreTaskCoordinate(const DecryptTask &task, const Coordinate &coordinate);
void ExecuteDecryptTask(const DecryptTask &task);
void ResetExecutionCaches();
} // namespace CoordDecrypt::detail
