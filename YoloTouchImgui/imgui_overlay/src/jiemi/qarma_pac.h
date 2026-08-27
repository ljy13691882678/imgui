#pragma once

#include <cstdint>

namespace CoordDecrypt
{
struct ApgaKey
{
    uint64_t lo;
    uint64_t hi;
};

// Complete QARMA5 result before PACGA applies its architectural high-32-bit mask.
uint64_t ComputeQarma5Pac(uint64_t data, uint64_t modifier, const ApgaKey &key);
bool ValidateQarma5Implementation();
} // namespace CoordDecrypt