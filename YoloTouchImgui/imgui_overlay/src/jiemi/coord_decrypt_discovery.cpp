#include "jiemi/coord_decrypt_internal.h"

#include "disassembler.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace CoordDecrypt::detail
{
namespace
{
constexpr uint64_t kGetterBridgeFieldOffset = 0xCULL;
constexpr uint64_t kTrampolineEntryFieldOffset = 0xA0ULL;
constexpr uint64_t kMaximumDynamicRegionSize = 4ULL * 1024ULL * 1024ULL;
using SnapshotPage = std::array<uint8_t, kPageSize>;

std::vector<std::string> SplitOperands(const char *text)
{
    std::vector<std::string> result;
    if (!text)
        return result;
    const char *start = text;
    for (const char *cursor = text;; ++cursor)
    {
        if (*cursor == ',' || *cursor == '\0')
        {
            const char *first = start;
            while (first < cursor && *first == ' ')
                ++first;
            const char *last = cursor;
            while (last > first && last[-1] == ' ')
                --last;
            result.emplace_back(first, static_cast<size_t>(last - first));
            if (*cursor == '\0')
                break;
            start = cursor + 1;
        }
    }
    return result;
}

std::string NormalizeRegister(std::string value)
{
    if (!value.empty() && value[0] == 'W')
        value[0] = 'X';
    return value;
}

bool IsControlFlow(const Disasm::DisasmLine &line)
{
    const std::string mnemonic = line.mnemonic;
    return mnemonic == "B" || mnemonic == "BR" || mnemonic == "BLR" ||
           mnemonic == "RET" || mnemonic == "CBZ" || mnemonic == "CBNZ" ||
           mnemonic == "TBZ" || mnemonic == "TBNZ" ||
           (mnemonic.size() > 2 && mnemonic[0] == 'B' && mnemonic[1] == '.');
}

bool WritesRegister(const Disasm::DisasmLine &line, const std::string &target)
{
    const auto operands = SplitOperands(line.op_str);
    return !operands.empty() &&
           NormalizeRegister(operands.front()) == NormalizeRegister(target);
}

bool ParseImmediate(const std::string &text, uint64_t &value)
{
    const size_t marker = text.find('#');
    if (marker == std::string::npos)
        return false;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str() + marker + 1, &end, 0);
    if (!end || end == text.c_str() + marker + 1)
        return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool ResolveRegisterImmediate(const std::vector<Disasm::DisasmLine> &lines,
                              size_t before, const std::string &target,
                              uint32_t &value)
{
    const size_t lower = before > 15 ? before - 15 : 0;
    bool ready = false;
    uint64_t current = 0;
    for (size_t index = lower; index < before; ++index)
    {
        const auto operands = SplitOperands(lines[index].op_str);
        if (operands.empty() ||
            NormalizeRegister(operands[0]) != NormalizeRegister(target))
            continue;
        uint64_t immediate = 0;
        if (std::strcmp(lines[index].mnemonic, "MOV") == 0 &&
            operands.size() >= 2 &&
            ParseImmediate(operands[1], immediate))
        {
            current = immediate;
            ready = true;
            continue;
        }
        if (std::strcmp(lines[index].mnemonic, "MOVK") == 0 && ready &&
            operands.size() >= 2 &&
            ParseImmediate(operands[1], immediate))
        {
            uint64_t shift = 0;
            if (operands.size() >= 3)
                ParseImmediate(operands[2], shift);
            const uint64_t mask = UINT64_C(0xFFFF) << shift;
            current = (current & ~mask) | ((immediate & 0xFFFFULL) << shift);
            continue;
        }
        if (WritesRegister(lines[index], target))
            ready = false;
    }
    if (!ready)
        return false;
    value = static_cast<uint32_t>(current);
    return true;
}

bool ReadRemotePointer(uint64_t address, uint64_t &pointer)
{
    uint64_t raw = 0;
    if (!CoordDriverBridge::Read(address, &raw, sizeof(raw)))
        return false;
    pointer = CanonicalAddress(raw);
    return IsRemoteAddress(pointer);
}

bool LoadDynamicRegion(uint64_t entry, uint64_t &base, std::vector<uint8_t> &data,
                       std::string &error)
{
    error.clear();
    const auto regions = CoordDriverBridge::GetScanRegions();
    for (const auto &[start, end] : regions)
    {
        if (entry < start || entry >= end)
            continue;
        const uint64_t size = end - start;
        if (size == 0 || size > kMaximumDynamicRegionSize)
        {
            char message[224]{};
            std::snprintf(message, sizeof(message),
                          "entry=0x%llX region=[0x%llX,0x%llX) size=0x%llX exceeds_limit=0x%llX scan_regions=%zu",
                          static_cast<unsigned long long>(entry),
                          static_cast<unsigned long long>(start),
                          static_cast<unsigned long long>(end),
                          static_cast<unsigned long long>(size),
                          static_cast<unsigned long long>(kMaximumDynamicRegionSize),
                          regions.size());
            error = message;
            return false;
        }
        data.assign(static_cast<size_t>(size), 0);
        bool entryPageRead = false;
        size_t pagesRead = 0;
        size_t pagesFailed = 0;
        for (uint64_t offset = 0; offset < size; offset += kPageSize)
        {
            const size_t chunk =
                static_cast<size_t>(std::min<uint64_t>(kPageSize, size - offset));
            const bool pageRead =
                CoordDriverBridge::Read(start + offset, data.data() + offset, chunk);
            if (pageRead)
                ++pagesRead;
            else
                ++pagesFailed;
            if (entry >= start + offset && entry < start + offset + chunk)
                entryPageRead = pageRead;
        }
        base = start;
        if (!entryPageRead)
        {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                          "entry=0x%llX region=[0x%llX,0x%llX) size=0x%llX entry_page=failed pages_ok=%zu pages_failed=%zu scan_regions=%zu",
                          static_cast<unsigned long long>(entry),
                          static_cast<unsigned long long>(start),
                          static_cast<unsigned long long>(end),
                          static_cast<unsigned long long>(size),
                          pagesRead, pagesFailed, regions.size());
            error = message;
        }
        return entryPageRead;
    }

    // Some driver builds omit anonymous executable mappings from GetScanRegions().
    // The dynamic entry is page-aligned in the current trampoline contract, so
    // mirror contiguous readable pages forward and stop at the first gap.
    base = entry & ~(kPageSize - 1);
    data.clear();
    data.reserve(static_cast<size_t>(kMaximumDynamicRegionSize));
    uint64_t firstFailedPage = 0;
    for (uint64_t offset = 0; offset < kMaximumDynamicRegionSize; offset += kPageSize)
    {
        SnapshotPage page{};
        if (!CoordDriverBridge::Read(base + offset, page.data(), page.size()))
        {
            firstFailedPage = base + offset;
            break;
        }
        data.insert(data.end(), page.begin(), page.end());
    }
    if (data.empty())
    {
        char message[224]{};
        std::snprintf(message, sizeof(message),
                      "entry=0x%llX no_containing_region scan_regions=%zu fallback_base=0x%llX first_page=failed",
                      static_cast<unsigned long long>(entry), regions.size(),
                      static_cast<unsigned long long>(base));
        error = message;
    }
    else if (firstFailedPage != 0)
    {
        char message[224]{};
        std::snprintf(message, sizeof(message),
                      "entry=0x%llX no_containing_region scan_regions=%zu fallback_base=0x%llX pages_ok=%zu first_failed=0x%llX",
                      static_cast<unsigned long long>(entry), regions.size(),
                      static_cast<unsigned long long>(base),
                      data.size() / kPageSize,
                      static_cast<unsigned long long>(firstFailedPage));
        error = message;
    }
    return !data.empty();
}

} // namespace

bool DiscoverRingServiceLayout(uint64_t getterAddress, RingServiceLayout &layout,
                               std::string &error)
{
    layout = {};
    error.clear();
    if (!ReadRemotePointer(getterAddress + kGetterBridgeFieldOffset, layout.shellcode))
    {
        error = "unable to resolve getter trampoline";
        return false;
    }
    if (!ReadRemotePointer(layout.shellcode + kTrampolineEntryFieldOffset,
                           layout.entry))
    {
        error = "unable to resolve dynamic entry";
        return false;
    }

    Disasm::Disassembler disassembler;
    if (!disassembler.IsValid())
    {
        error = "Capstone ARM64 initialization failed";
        return false;
    }

    uint64_t regionBase = 0;
    std::vector<uint8_t> region;
    std::string regionError;
    if (!LoadDynamicRegion(layout.entry, regionBase, region, regionError))
    {
        error = "unable to read dynamic entry region: " + regionError;
        return false;
    }
    layout.regionBase = regionBase;
    layout.regionSize = region.size();
    if (layout.entry < regionBase ||
        layout.entry - regionBase >= region.size())
    {
        error = "dynamic entry is outside the mirrored region";
        return false;
    }
    const size_t entryOffset = static_cast<size_t>(layout.entry - regionBase);
    const auto lines =
        disassembler.Disassemble(layout.entry, region.data() + entryOffset,
                                 region.size() - entryOffset, 0);
    if (lines.empty())
    {
        error = "dynamic entry disassembly is empty";
        return false;
    }

    struct HashCandidate
    {
        size_t instruction = 0;
        size_t store = 0;
    };
    struct RingCandidate
    {
        size_t instruction = 0;
        uint64_t immediate = 0;
        std::string multiplier;
    };
    std::vector<HashCandidate> hashCandidates;
    std::vector<RingCandidate> ringCandidates;
    for (size_t index = 0; index < lines.size(); ++index)
    {
        const std::string mnemonic = lines[index].mnemonic;
        if (mnemonic != "MADD" && mnemonic != "SMADDL" && mnemonic != "UMADDL")
            continue;
        const auto operands = SplitOperands(lines[index].op_str);
        if (operands.size() < 4)
            continue;
        const std::string destination = NormalizeRegister(operands[0]);
        const std::string multiplier = NormalizeRegister(operands[2]);
        uint64_t immediate = 0;
        bool immediateReady = false;
        const size_t lower = index > 60 ? index - 60 : 0;
        for (size_t previous = index; previous-- > lower;)
        {
            if (IsControlFlow(lines[previous]))
                break;
            const auto previousOperands = SplitOperands(lines[previous].op_str);
            if (std::strcmp(lines[previous].mnemonic, "MOV") == 0 &&
                previousOperands.size() >= 2 &&
                NormalizeRegister(previousOperands[0]) == multiplier &&
                ParseImmediate(previousOperands[1], immediate))
            {
                immediateReady = true;
                break;
            }
            if (WritesRegister(lines[previous], multiplier))
                break;
        }
        if (!immediateReady)
            continue;
        if (immediate != 0 && immediate <= 0x1000)
            ringCandidates.push_back({index, immediate, multiplier});

        const size_t upper = std::min(lines.size(), index + 21);
        for (size_t following = index + 1; following < upper; ++following)
        {
            if (IsControlFlow(lines[following]))
                break;
            const auto followingOperands = SplitOperands(lines[following].op_str);
            if ((std::strcmp(lines[following].mnemonic, "STR") == 0 ||
                 std::strcmp(lines[following].mnemonic, "STP") == 0) &&
                std::strstr(lines[following].op_str, "[SP") &&
                std::any_of(followingOperands.begin(), followingOperands.end() - 1,
                            [&](const std::string &operand)
                            { return NormalizeRegister(operand) == destination; }))
            {
                hashCandidates.push_back({index, following});
                break;
            }
            if (WritesRegister(lines[following], destination))
                break;
        }
    }

    const HashCandidate *selectedHash = nullptr;
    for (const HashCandidate &candidate : hashCandidates)
    {
        if (std::strcmp(lines[candidate.instruction].mnemonic, "MADD") == 0)
        {
            selectedHash = &candidate;
            break;
        }
    }
    if (!selectedHash && !hashCandidates.empty())
        selectedHash = &hashCandidates.back();
    const RingCandidate *selectedRing = nullptr;
    for (size_t leftIndex = 0; leftIndex < ringCandidates.size(); ++leftIndex)
    {
        const RingCandidate &left = ringCandidates[leftIndex];
        for (size_t rightIndex = leftIndex + 1; rightIndex < ringCandidates.size();
             ++rightIndex)
        {
            const RingCandidate &right = ringCandidates[rightIndex];
            if (right.instruction <= left.instruction)
                continue;
            const size_t distance = right.instruction - left.instruction;
            if (distance > 16)
                break;
            if (left.immediate != right.immediate ||
                left.multiplier != right.multiplier)
                continue;
            bool crossesBranch = false;
            for (size_t index = left.instruction + 1; index < right.instruction;
                 ++index)
            {
                if (IsControlFlow(lines[index]))
                {
                    crossesBranch = true;
                    break;
                }
            }
            if (!crossesBranch)
            {
                selectedRing = &left;
                break;
            }
        }
        if (selectedRing)
            break;
    }
    if (!selectedHash || !selectedRing)
    {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "unable to discover hashEnd/ring stride hash_candidates=%zu ring_candidates=%zu",
                      hashCandidates.size(), ringCandidates.size());
        error = message;
        return false;
    }

    const auto hashOperands = SplitOperands(lines[selectedHash->instruction].op_str);
    const std::string hashRegister = NormalizeRegister(hashOperands[0]);
    if (hashRegister.size() < 2 || hashRegister[0] != 'X')
    {
        error = "hashEnd output is not a GP register";
        return false;
    }
    layout.hashRegister =
        static_cast<uint32_t>(std::strtoul(hashRegister.c_str() + 1, nullptr, 10));
    layout.hashEnd = lines[selectedHash->store].address;

    const size_t ringIndex = selectedRing->instruction;
    const auto ringOperands = SplitOperands(lines[ringIndex].op_str);
    layout.ringStride = selectedRing->immediate;
    const std::string accumulator = NormalizeRegister(ringOperands[3]);
    const size_t ringLower = ringIndex > 50 ? ringIndex - 50 : 0;
    for (size_t previous = ringIndex; previous-- > ringLower;)
    {
        if (IsControlFlow(lines[previous]))
            break;
        const auto previousOperands = SplitOperands(lines[previous].op_str);
        uint64_t immediate = 0;
        if (std::strcmp(lines[previous].mnemonic, "ADD") == 0 &&
            previousOperands.size() >= 3 &&
            NormalizeRegister(previousOperands[0]) == accumulator &&
            ParseImmediate(previousOperands[2], immediate))
        {
            layout.ringOffset = immediate;
            break;
        }
    }
    std::vector<uint64_t> svcAddresses;
    for (const auto &line : lines)
    {
        if (std::strcmp(line.mnemonic, "SVC") == 0)
            svcAddresses.push_back(line.address);
    }
    if (svcAddresses.size() != 1)
    {
        error = "dynamic entry does not contain exactly one SVC";
        return false;
    }
    layout.svc = svcAddresses.front();

    {
        std::unordered_map<uint64_t, size_t> branchTargetCounts;
        for (const auto &line : lines)
        {
            if (std::strcmp(line.mnemonic, "B") != 0)
                continue;
            uint64_t target = 0;
            if (ParseImmediate(line.op_str, target) &&
                target >= layout.entry &&
                target < layout.entry + region.size() - entryOffset)
                ++branchTargetCounts[target];
        }
        size_t maximumReferences = 0;
        for (const auto &[target, count] : branchTargetCounts)
        {
            if (count > maximumReferences)
            {
                maximumReferences = count;
                layout.dispatcher = target;
            }
        }

        if (layout.dispatcher != 0)
        {
            const auto dispatcher = std::find_if(
                lines.begin(), lines.end(),
                [&](const Disasm::DisasmLine &line)
                { return line.address == layout.dispatcher; });
            uint64_t stateSlotOffset = 0;
            if (dispatcher == lines.end() ||
                std::strcmp(dispatcher->mnemonic, "LDR") != 0 ||
                std::strstr(dispatcher->op_str, "[SP") == nullptr ||
                !ParseImmediate(dispatcher->op_str, stateSlotOffset) ||
                stateSlotOffset > 0x1000)
            {
                layout.dispatcher = 0;
            }
            else
            {
                layout.stateSlotOffset =
                    static_cast<uint32_t>(stateSlotOffset);
            }
        }

        std::unordered_set<uint64_t> seenGuards;
        for (size_t index = 0; index < lines.size(); ++index)
        {
            if (std::strcmp(lines[index].mnemonic, "B.NE") != 0)
                continue;
            uint64_t branchTarget = 0;
            if (!ParseImmediate(lines[index].op_str, branchTarget) ||
                branchTarget != layout.dispatcher || index == 0 ||
                std::strcmp(lines[index - 1].mnemonic, "CMP") != 0)
                continue;
            const auto compareOperands =
                SplitOperands(lines[index - 1].op_str);
            if (compareOperands.size() < 2 ||
                NormalizeRegister(compareOperands[0]) != "X8")
                continue;
            uint32_t selector = 0;
            if (!ResolveRegisterImmediate(
                    lines, index - 1, compareOperands[1], selector) ||
                selector == 0)
                continue;

            const size_t upper =
                std::min(lines.size(), index + 51);
            for (size_t following = index + 1;
                 following < upper; ++following)
            {
                if (IsControlFlow(lines[following]))
                    break;
                if (std::strcmp(lines[following].mnemonic, "LDR") != 0 ||
                    std::strstr(lines[following].op_str, "[SP") == nullptr)
                    continue;
                const auto loadOperands =
                    SplitOperands(lines[following].op_str);
                if (loadOperands.empty() ||
                    loadOperands[0].empty() ||
                    loadOperands[0][0] != 'X')
                    continue;
                uint64_t stackOffset = 0;
                if (!ParseImmediate(
                        lines[following].op_str, stackOffset) ||
                    stackOffset > 0x1000)
                    continue;
                const uint64_t key =
                    (stackOffset << 32) | selector;
                if (seenGuards.insert(key).second)
                    layout.stateGuards.push_back(
                        {static_cast<uint32_t>(stackOffset),
                         selector});
            }
        }

        std::unordered_set<uint64_t> seenLegacyGuards;
        for (size_t index = 0; index + 1 < lines.size(); ++index)
        {
            if (std::strcmp(lines[index].mnemonic, "B") != 0)
                continue;
            uint64_t branchTarget = 0;
            if (!ParseImmediate(lines[index].op_str, branchTarget) ||
                branchTarget != layout.dispatcher)
                continue;
            const uint64_t legacyEntry = lines[index + 1].address;
            const size_t upper =
                std::min(lines.size(), index + 51);
            for (size_t following = index + 1;
                 following < upper; ++following)
            {
                if (following != index + 1 &&
                    IsControlFlow(lines[following]))
                    break;
                if (std::strcmp(lines[following].mnemonic, "LDR") != 0 ||
                    std::strstr(lines[following].op_str, "[SP") == nullptr)
                    continue;
                const auto loadOperands =
                    SplitOperands(lines[following].op_str);
                if (loadOperands.empty() ||
                    loadOperands[0].empty() ||
                    loadOperands[0][0] != 'X')
                    continue;
                uint64_t stackOffset = 0;
                if (!ParseImmediate(
                        lines[following].op_str, stackOffset) ||
                    stackOffset > 0x1000)
                    continue;
                const uint64_t key =
                    (stackOffset << 32) ^
                    (legacyEntry & UINT64_C(0xFFFFFFFF));
                if (seenLegacyGuards.insert(key).second)
                    layout.legacyGuards.push_back(
                        {static_cast<uint32_t>(stackOffset),
                         legacyEntry});
            }
        }
    }

    layout.ringCalc = !layout.legacyGuards.empty()
                          ? layout.legacyGuards.front().entry
                          : layout.dispatcher;
    if (!IsRemoteAddress(layout.dispatcher) ||
        layout.stateSlotOffset > 0x1000 ||
        (layout.stateGuards.empty() &&
         layout.legacyGuards.empty()) ||
        layout.hashRegister >= 31)
    {
        error = "ring service discovery produced invalid output";
        return false;
    }
    layout.valid = true;
    return true;
}

} // namespace CoordDecrypt::detail
