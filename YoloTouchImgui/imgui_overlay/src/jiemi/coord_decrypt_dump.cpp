#include "jiemi/coord_decrypt.h"
#include "jiemi/coord_decrypt_internal.h"
#include "jiemi/coord_driver_bridge.h"

#include <array>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace CoordDecrypt
{
using namespace detail;

ShellcodeDumpResult DumpShellcode(uint64_t libUE4Base)
{
    ShellcodeDumpResult result{};
    if (libUE4Base == 0)
    {
        result.message = "libUE4 is not bound";
        return result;
    }

    constexpr size_t kDumpSize = 0x1000;
    std::array<uint8_t, kDumpSize> getterPage{};
    const uint64_t getterAddress = libUE4Base + kGetterOffset;
    if (!CoordDriverBridge::Read(getterAddress, getterPage.data(), getterPage.size()))
    {
        result.message = "unable to read coordinate getter page";
        return result;
    }

    const char *directory = "/sdcard/shellcode";
    const char *path = "/sdcard/shellcode/coord_getter.bin";
    mkdir(directory, 0755);
    const int fileDescriptor = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fileDescriptor < 0)
    {
        result.message = "unable to create /sdcard/shellcode/coord_getter.bin";
        return result;
    }

    const ssize_t written = write(fileDescriptor, getterPage.data(), getterPage.size());
    close(fileDescriptor);
    if (written != static_cast<ssize_t>(getterPage.size()))
    {
        result.message = "coordinate getter dump was incomplete";
        return result;
    }

    result.success = true;
    result.message = "saved /sdcard/shellcode/coord_getter.bin";
    return result;
}

} // namespace CoordDecrypt
