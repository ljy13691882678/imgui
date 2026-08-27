/*
 * Minimal QARMA5 implementation adapted from QEMU
 * target/arm/tcg/pauth_helper.c, LGPL-2.1-or-later.
 * It intentionally omits QEMU's implementation-defined hash fallback.
 */

#include "jiemi/qarma_pac.h"

#include <array>
#include <cstdint>

namespace CoordDecrypt
{
namespace
{
uint64_t Extract64(uint64_t value, unsigned start, unsigned length)
{
    return length == 64 ? value : (value >> start) & ((UINT64_C(1) << length) - UINT64_C(1));
}

uint32_t Extract32(uint32_t value, unsigned start, unsigned length)
{
    return length == 32 ? value : (value >> start) & ((UINT32_C(1) << length) - UINT32_C(1));
}

uint64_t CellShuffle(uint64_t input)
{
    uint64_t output = 0;
    output |= Extract64(input, 52, 4);
    output |= Extract64(input, 24, 4) << 4;
    output |= Extract64(input, 44, 4) << 8;
    output |= Extract64(input, 0, 4) << 12;
    output |= Extract64(input, 28, 4) << 16;
    output |= Extract64(input, 48, 4) << 20;
    output |= Extract64(input, 4, 4) << 24;
    output |= Extract64(input, 40, 4) << 28;
    output |= Extract64(input, 32, 4) << 32;
    output |= Extract64(input, 12, 4) << 36;
    output |= Extract64(input, 56, 4) << 40;
    output |= Extract64(input, 20, 4) << 44;
    output |= Extract64(input, 8, 4) << 48;
    output |= Extract64(input, 36, 4) << 52;
    output |= Extract64(input, 16, 4) << 56;
    output |= Extract64(input, 60, 4) << 60;
    return output;
}

uint64_t CellInverseShuffle(uint64_t input)
{
    uint64_t output = 0;
    output |= Extract64(input, 12, 4);
    output |= Extract64(input, 24, 4) << 4;
    output |= Extract64(input, 48, 4) << 8;
    output |= Extract64(input, 36, 4) << 12;
    output |= Extract64(input, 56, 4) << 16;
    output |= Extract64(input, 44, 4) << 20;
    output |= Extract64(input, 4, 4) << 24;
    output |= Extract64(input, 16, 4) << 28;
    output |= input & (UINT64_C(0xF) << 32);
    output |= Extract64(input, 52, 4) << 36;
    output |= Extract64(input, 28, 4) << 40;
    output |= Extract64(input, 8, 4) << 44;
    output |= Extract64(input, 20, 4) << 48;
    output |= Extract64(input, 0, 4) << 52;
    output |= Extract64(input, 40, 4) << 56;
    output |= input & (UINT64_C(0xF) << 60);
    return output;
}

uint64_t Substitute(uint64_t input)
{
    static constexpr std::array<uint8_t, 16> table{
        0xB, 0x6, 0x8, 0xF, 0xC, 0x0, 0x9, 0xE,
        0x3, 0x7, 0x4, 0x5, 0xD, 0x2, 0x1, 0xA,
    };
    uint64_t output = 0;
    for (unsigned bit = 0; bit < 64; bit += 4)
        output |= static_cast<uint64_t>(table[(input >> bit) & 0xF]) << bit;
    return output;
}

uint64_t InverseSubstitute(uint64_t input)
{
    static constexpr std::array<uint8_t, 16> table{
        0x5, 0xE, 0xD, 0x8, 0xA, 0xB, 0x1, 0x9,
        0x2, 0x6, 0xF, 0x0, 0x4, 0xC, 0x7, 0x3,
    };
    uint64_t output = 0;
    for (unsigned bit = 0; bit < 64; bit += 4)
        output |= static_cast<uint64_t>(table[(input >> bit) & 0xF]) << bit;
    return output;
}

int RotateCell(int cell, int amount)
{
    cell |= cell << 4;
    return static_cast<int>(Extract32(static_cast<uint32_t>(cell), static_cast<unsigned>(4 - amount), 4));
}

uint64_t Multiply(uint64_t input)
{
    uint64_t output = 0;
    for (unsigned bit = 0; bit < 16; bit += 4)
    {
        const int i0 = static_cast<int>(Extract64(input, bit, 4));
        const int i4 = static_cast<int>(Extract64(input, bit + 16, 4));
        const int i8 = static_cast<int>(Extract64(input, bit + 32, 4));
        const int ic = static_cast<int>(Extract64(input, bit + 48, 4));
        const int t0 = RotateCell(i8, 1) ^ RotateCell(i4, 2) ^ RotateCell(i0, 1);
        const int t1 = RotateCell(ic, 1) ^ RotateCell(i4, 1) ^ RotateCell(i0, 2);
        const int t2 = RotateCell(ic, 2) ^ RotateCell(i8, 1) ^ RotateCell(i0, 1);
        const int t3 = RotateCell(ic, 1) ^ RotateCell(i8, 2) ^ RotateCell(i4, 1);
        output |= static_cast<uint64_t>(t3) << bit;
        output |= static_cast<uint64_t>(t2) << (bit + 16);
        output |= static_cast<uint64_t>(t1) << (bit + 32);
        output |= static_cast<uint64_t>(t0) << (bit + 48);
    }
    return output;
}

uint64_t TweakCellRotate(uint64_t cell)
{
    return (cell >> 1) | (((cell ^ (cell >> 1)) & 1) << 3);
}

uint64_t TweakShuffle(uint64_t input)
{
    uint64_t output = 0;
    output |= Extract64(input, 16, 4);
    output |= Extract64(input, 20, 4) << 4;
    output |= TweakCellRotate(Extract64(input, 24, 4)) << 8;
    output |= Extract64(input, 28, 4) << 12;
    output |= TweakCellRotate(Extract64(input, 44, 4)) << 16;
    output |= Extract64(input, 8, 4) << 20;
    output |= Extract64(input, 12, 4) << 24;
    output |= TweakCellRotate(Extract64(input, 32, 4)) << 28;
    output |= Extract64(input, 48, 4) << 32;
    output |= Extract64(input, 52, 4) << 36;
    output |= Extract64(input, 56, 4) << 40;
    output |= TweakCellRotate(Extract64(input, 60, 4)) << 44;
    output |= TweakCellRotate(Extract64(input, 0, 4)) << 48;
    output |= Extract64(input, 4, 4) << 52;
    output |= TweakCellRotate(Extract64(input, 40, 4)) << 56;
    output |= TweakCellRotate(Extract64(input, 36, 4)) << 60;
    return output;
}

uint64_t TweakCellInverseRotate(uint64_t cell)
{
    return ((cell << 1) & 0xF) | ((cell & 1) ^ (cell >> 3));
}

uint64_t TweakInverseShuffle(uint64_t input)
{
    uint64_t output = 0;
    output |= TweakCellInverseRotate(Extract64(input, 48, 4));
    output |= Extract64(input, 52, 4) << 4;
    output |= Extract64(input, 20, 4) << 8;
    output |= Extract64(input, 24, 4) << 12;
    output |= Extract64(input, 0, 4) << 16;
    output |= Extract64(input, 4, 4) << 20;
    output |= TweakCellInverseRotate(Extract64(input, 8, 4)) << 24;
    output |= Extract64(input, 12, 4) << 28;
    output |= TweakCellInverseRotate(Extract64(input, 28, 4)) << 32;
    output |= TweakCellInverseRotate(Extract64(input, 60, 4)) << 36;
    output |= TweakCellInverseRotate(Extract64(input, 56, 4)) << 40;
    output |= TweakCellInverseRotate(Extract64(input, 16, 4)) << 44;
    output |= Extract64(input, 32, 4) << 48;
    output |= Extract64(input, 36, 4) << 52;
    output |= Extract64(input, 40, 4) << 56;
    output |= TweakCellInverseRotate(Extract64(input, 44, 4)) << 60;
    return output;
}
} // namespace

uint64_t ComputeQarma5Pac(uint64_t data, uint64_t modifier, const ApgaKey &key)
{
    static constexpr std::array<uint64_t, 5> roundConstants{
        UINT64_C(0x0000000000000000), UINT64_C(0x13198A2E03707344),
        UINT64_C(0xA4093822299F31D0), UINT64_C(0x082EFA98EC4E6C89),
        UINT64_C(0x452821E638D01377),
    };
    constexpr uint64_t alpha = UINT64_C(0xC0AC29B7C97C50DD);
    constexpr int iterations = 4;
    const uint64_t key0 = key.hi;
    const uint64_t key1 = key.lo;
    const uint64_t modifiedKey0 = (key0 << 63) | ((key0 >> 1) ^ (key0 >> 63));
    uint64_t runningModifier = modifier;
    uint64_t working = data ^ key0;

    for (int index = 0; index <= iterations; ++index)
    {
        working ^= key1 ^ runningModifier;
        working ^= roundConstants[index];
        if (index > 0)
        {
            working = CellShuffle(working);
            working = Multiply(working);
        }
        working = Substitute(working);
        runningModifier = TweakShuffle(runningModifier);
    }

    working ^= modifiedKey0 ^ runningModifier;
    working = CellShuffle(working);
    working = Multiply(working);
    working = Substitute(working);
    working = CellShuffle(working);
    working = Multiply(working);
    working ^= key1;
    working = CellInverseShuffle(working);
    working = InverseSubstitute(working);
    working = Multiply(working);
    working = CellInverseShuffle(working);
    working ^= key0 ^ runningModifier;

    for (int index = 0; index <= iterations; ++index)
    {
        working = InverseSubstitute(working);
        if (index < iterations)
        {
            working = Multiply(working);
            working = CellInverseShuffle(working);
        }
        runningModifier = TweakInverseShuffle(runningModifier);
        working ^= roundConstants[iterations - index];
        working ^= key1 ^ runningModifier;
        working ^= alpha;
    }
    return working ^ modifiedKey0;
}

bool ValidateQarma5Implementation()
{
    constexpr ApgaKey key{UINT64_C(0xEC2802D4E0A488E9), UINT64_C(0x84BE85CE9804E94B)};
    return ComputeQarma5Pac(UINT64_C(0xFB623599DA6E8127), UINT64_C(0x477D469DEC0B8762), key) ==
           UINT64_C(0xC003B93999B33765);
}
} // namespace CoordDecrypt