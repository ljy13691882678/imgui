#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace CoordDriverBridge
{
struct Environment
{
    int processId = 0;
    uint64_t tls = 0;
    uint64_t pacgaLo = 0;
    uint64_t pacgaHi = 0;
    uint64_t tid = 0;
};

bool Read(uint64_t address, void *buffer, size_t size);
std::vector<std::pair<uintptr_t, uintptr_t>> GetScanRegions();
bool GetGameThreadEnvironment(Environment &environment);
bool GetFileDescriptorSize(int processId, uint64_t fileDescriptor, uint64_t &size);
} // namespace CoordDriverBridge