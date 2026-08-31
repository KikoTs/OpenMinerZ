#include "openminerz/asset_paths.hpp"

#include <array>
#include <cstdlib>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace openminerz
{
namespace
{

AssetStyle selected_style = AssetStyle::modern;

std::filesystem::path find_executable_directory()
{
#if defined(_WIN32)
    std::wstring module_path(32768U, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length != 0U && length < module_path.size())
    {
        module_path.resize(length);
        return std::filesystem::path{module_path}.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path local_pack_root()
{
#if defined(_WIN32)
    char *local_data{};
    std::size_t local_data_size{};
    if (_dupenv_s(&local_data, &local_data_size, "LOCALAPPDATA") == 0 && local_data != nullptr)
    {
        const std::filesystem::path result =
            std::filesystem::path{local_data} / "OpenMinerZ" / "ContentPacks";
        std::free(local_data);
        return result;
    }
#endif
    return std::filesystem::current_path() / "content-packs";
}

std::string_view style_directory() noexcept
{
    switch (selected_style)
    {
    case AssetStyle::classic:
        return "classic";
    case AssetStyle::classic_hd:
        return "classic-hd";
    case AssetStyle::modern:
        return "modern";
    }
    return "modern";
}

bool usable_pack(const std::filesystem::path &path)
{
    std::error_code error;
    return std::filesystem::exists(path / "DerivedData" / "terrain_diffuse.bmp", error) &&
           std::filesystem::exists(path / "Content" / "Sounds.xgs", error);
}

} // namespace

void set_active_asset_style(AssetStyle style) noexcept
{
    selected_style = style;
}

AssetStyle active_asset_style() noexcept
{
    return selected_style;
}

std::filesystem::path active_content_pack_path()
{
    const auto executable = application_directory();
    const std::array candidates{
        executable / "ContentPacks" / style_directory(),
        executable.parent_path() / "content-packs" / style_directory(),
        local_pack_root() / style_directory(),
    };
    for (const auto &candidate : candidates)
    {
        if (usable_pack(candidate))
            return candidate;
    }
    // Return the canonical build-tree location even when import has not run so
    // diagnostics identify the missing pack rather than silently mixing styles.
    return candidates[1];
}

std::filesystem::path active_derived_data_path()
{
    const auto pack = active_content_pack_path();
    std::error_code error;
    const auto derived = pack / "DerivedData";
    if (std::filesystem::exists(derived / "terrain_diffuse.bmp", error))
        return derived;

    // One migration fallback for pre-pack installations. It is Modern because
    // the former single cache was always produced from the current Steam build.
    if (selected_style == AssetStyle::modern)
    {
#if defined(_WIN32)
        char *local_data{};
        std::size_t local_data_size{};
        if (_dupenv_s(&local_data, &local_data_size, "LOCALAPPDATA") == 0 && local_data != nullptr)
        {
            const auto legacy = std::filesystem::path{local_data} / "OpenMinerZ" / "DerivedData";
            std::free(local_data);
            if (std::filesystem::exists(legacy / "terrain_diffuse.bmp", error))
                return legacy;
        }
#endif
    }
    return derived;
}

std::filesystem::path application_directory()
{
    return find_executable_directory();
}

std::filesystem::path installed_steam_api_path()
{
    std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
    wchar_t *override_path{};
    std::size_t override_size{};
    if (_wdupenv_s(&override_path, &override_size, L"OPENMINERZ_STEAM_API") == 0 &&
        override_path != nullptr)
    {
        candidates.emplace_back(override_path);
        std::free(override_path);
    }
#endif
    candidates.push_back(application_directory() / "steam_api.dll");
#if defined(OPENMINERZ_OWNED_GAME_DIR)
    candidates.push_back(std::filesystem::path{OPENMINERZ_OWNED_GAME_DIR} / "steam_api.dll");
#endif
#if defined(_WIN32)
    candidates.emplace_back(
        LR"(C:\Program Files (x86)\Steam\steamapps\common\CastleMiner Z\steam_api.dll)");
#endif
    std::error_code error;
    for (const std::filesystem::path &candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            const std::filesystem::path canonical = std::filesystem::canonical(candidate, error);
            if (!error)
                return canonical;
        }
        error.clear();
    }
    return {};
}

} // namespace openminerz
