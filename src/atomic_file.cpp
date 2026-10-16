#include "openminerz/atomic_file.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace openminerz
{
bool write_file_atomically(const std::filesystem::path &path, std::span<const std::uint8_t> bytes)
{
    if (path.empty())
        return false;
    static std::atomic<unsigned long long> sequence{};
    auto temporary = path;
    temporary += ".tmp-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence.fetch_add(1));
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    bool success = false;
#if defined(_WIN32)
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    success = true;
    while (!bytes.empty() && success)
    {
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1024U * 1024U));
        DWORD written{};
        success = WriteFile(file, bytes.data(), count, &written, nullptr) && written == count;
        if (success)
            bytes = bytes.subspan(written);
    }
    success = success && FlushFileBuffers(file);
    success = CloseHandle(file) && success;
    if (success)
        success = MoveFileExW(temporary.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        success = stream.good();
        stream.close();
        success = success && !stream.fail();
    }
    if (success)
    {
        std::filesystem::rename(temporary, path, error);
        success = !error;
    }
#endif
    if (!success)
        std::filesystem::remove(temporary, error);
    return success;
}
} // namespace openminerz
