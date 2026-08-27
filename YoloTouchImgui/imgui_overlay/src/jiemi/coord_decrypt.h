#pragma once

#include <cstdint>
#include <string>

namespace CoordDecrypt
{
struct Coordinate
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ShellcodeDumpResult
{
    bool success = false;
    std::string message;
};

void Tick(bool enabled, uint64_t libUE4Base);
void Reset();
void Shutdown();
bool TryRead(uint64_t sceneComponent, Coordinate &coordinate);
bool TryReadSmoothed(uint64_t sceneComponent, Coordinate &coordinate);
ShellcodeDumpResult DumpShellcode(uint64_t libUE4Base);
} // namespace CoordDecrypt