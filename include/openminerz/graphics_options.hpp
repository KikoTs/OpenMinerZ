#pragma once

// The settings-menu rows, persistence keys and value handling of every option
// in GraphicsSettings, driven by one table so an option cannot have a row
// without a saved value, or a saved value without a row.

#include "openminerz/settings_menu.hpp"

#include <array>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::ui
{

enum class GraphicsOptionStorage
{
    flag,
    quality,
    focus_mode,
    scalar,
    integer,
    // The far-terrain distance: seven named choices (FarTerrainDistance).
    far_distance
};

// How a scalar is written on its row.
enum class GraphicsValueFormat
{
    percent,       // 0.30 -> "30%"
    percent_or_off, // 0 -> "OFF"
    temperature,   // -0.3 -> "COOL 30%", 0 -> "NEUTRAL", 0.3 -> "WARM 30%"
    signed_steps,  // -0.5 -> "-0.50", 0 -> "0", 0.5 -> "+0.50"
    blocks         // 150 -> "150 BLOCKS"
};

struct GraphicsOption final
{
    SettingId id{};
    SettingsTab tab{};
    // The row on the same page this one is listed after.
    SettingId after{};
    // Key in settings.cfg.
    std::string_view key;
    std::string_view label;
    std::string_view description;
    SettingKind kind{};
    // Set by the Low/Medium/High/Ultra preset (and so able to turn it Custom).
    bool preset_controlled{};

    GraphicsOptionStorage storage{};
    bool GraphicsSettings::*flag{};
    EffectQuality GraphicsSettings::*quality{};
    // False for a quality that has its own on/off row elsewhere.
    bool quality_allows_off{true};
    // What the five levels are called on the row when they are not OFF, LOW,
    // MEDIUM, HIGH, ULTRA. Empty for those.
    std::array<std::string_view, 5> quality_labels{};
    DepthOfFieldMode GraphicsSettings::*focus_mode{};
    FarTerrainDistance GraphicsSettings::*far_distance{};
    float GraphicsSettings::*scalar{};
    int GraphicsSettings::*integer{};
    float minimum{};
    float maximum{1.0F};
    float step{0.05F};
    GraphicsValueFormat format{GraphicsValueFormat::percent};
    // Non-empty while the option cannot act; see setting_unavailable_reason().
    std::string_view (*unavailable)(const Settings &) noexcept {};
};

[[nodiscard]] std::span<const GraphicsOption> graphics_options();
[[nodiscard]] const GraphicsOption *graphics_option(SettingId id);

// `rows` with every table option of `tab` inserted after its anchor row.
[[nodiscard]] std::vector<SettingRow> with_graphics_options(SettingsTab tab,
                                                            std::vector<SettingRow> rows);

[[nodiscard]] std::string graphics_option_value(const GraphicsOption &option,
                                                const Settings &settings);
[[nodiscard]] float graphics_option_normalized(const GraphicsOption &option,
                                               const Settings &settings) noexcept;
void adjust_graphics_option(const GraphicsOption &option, Settings &settings, int direction);
void reset_graphics_option(const GraphicsOption &option, Settings &settings);

// settings.cfg. A key missing from the file keeps the factory default, so a
// file written before these options existed loads with the intended look.
void load_graphics_options(Settings &settings,
                           const std::unordered_map<std::string, std::string> &values);
void save_graphics_options(const Settings &settings, std::ostream &output);

} // namespace openminerz::ui
