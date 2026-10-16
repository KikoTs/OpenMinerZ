#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace openminerz
{
// A failed write leaves the previous complete save in place. The temporary file
// is a sibling so replacement cannot cross filesystem/volume boundaries.
[[nodiscard]] bool write_file_atomically(const std::filesystem::path &path,
                                         std::span<const std::uint8_t> bytes);
} // namespace openminerz
