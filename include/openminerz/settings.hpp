#pragma once

#include <filesystem>
#include <string>

namespace openminerz
{

enum class WindowMode
{
    windowed,
    borderless,
    fullscreen
};
enum class PresentationProfile
{
    classic,
    rebirth
};
enum class AssetStyle
{
    classic,
    modern
};
enum class QualityLevel
{
    low,
    medium,
    high,
    ultra
};
enum class AntiAliasing
{
    off,
    fxaa,
    taa,
    msaa4x
};
// How the HDR scene is compressed to the display. "legacy" is the old
// soft-knee clamp that blew out bright terrain; the rest are real tone curves.
enum class ToneMapping
{
    legacy,
    reinhard,
    filmic,
    aces
};
enum class ColorFilter
{
    none,
    deuteranopia,
    protanopia,
    tritanopia
};

struct Settings final
{
    int schema_version{1};

    WindowMode window_mode{WindowMode::windowed};
    int window_width{1600};
    int window_height{900};
    bool vsync{true};
    int frame_limit{144};
    float ui_scale{1.0F};
    float safe_area{0.96F};

    PresentationProfile presentation{PresentationProfile::rebirth};
    // Selects a complete original-content generation at the next process start.
    AssetStyle texture_style{AssetStyle::modern};
    float render_scale{1.0F};
    QualityLevel texture_quality{QualityLevel::high};
    QualityLevel shadow_quality{QualityLevel::high};
    AntiAliasing anti_aliasing{AntiAliasing::taa};
    bool ambient_occlusion{true};
    QualityLevel particle_quality{QualityLevel::high};
    // Tone mapping and its manual exposure trim (stops, -2..+2). Auto exposure
    // measures the scene each frame; this biases the result.
    ToneMapping tone_mapping{ToneMapping::aces};
    float exposure_compensation{0.0F};
    bool auto_exposure{true};
    // Post-grade strength: 0 disables the saturation/vibrance/lift pass.
    float color_grading{1.0F};
    int view_distance{16};
    float vertical_fov{74.0F};
    // CastleMiner Z's Avatar.EyePointCamera uses a 90-degree projection.
    float viewmodel_fov{90.0F};

    float mouse_sensitivity{1.0F};
    float controller_sensitivity{1.0F};
    bool invert_y{false};
    bool auto_climb{true};
    // Creative-mode horizontal cruise speed in metres per second. Vertical
    // flight keeps the original 3:4 ratio so one control tunes both axes.
    float creative_flight_speed{8.0F};
    float camera_bob{0.65F};
    float screen_shake{0.75F};
    bool hud_visible{true};
    // Crafting pane style: false = classic discovery list, true = category browser.
    bool crafting_menu_modern{false};
    // Last chosen game mode / difficulty (original GameModeTypes /
    // GameDifficultyTypes enum indices; not persisted per world, same as the
    // original menu flow which re-asks on every launch but remembers cursor).
    int game_mode{1};
    int game_difficulty{1};

    float master_volume{0.90F};
    float music_volume{0.65F};
    float effects_volume{0.90F};
    float ambience_volume{0.80F};
    float voice_volume{0.85F};
    bool mute_unfocused{false};
    // Empty id follows the Windows default render endpoint. A named endpoint
    // is selected at the next process start so XACT can preload one renderer.
    std::string audio_output_id;
    std::string audio_output_name;

    bool raw_mouse_input{true};
    bool controller_vibration{true};
    bool controller_aim_assist{true};
    bool hold_to_sprint{false};

    bool reduced_motion{false};
    bool high_contrast{false};
    bool larger_text{false};
    ColorFilter color_filter{ColorFilter::none};
    bool subtitles{true};
    float subtitle_size{1.0F};

    void normalize() noexcept;
};

class SettingsStore final
{
  public:
    explicit SettingsStore(std::filesystem::path path);

    [[nodiscard]] Settings load() const;
    void save(const Settings &settings) const;
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string to_string(WindowMode value);
[[nodiscard]] std::string to_string(PresentationProfile value);
[[nodiscard]] std::string to_string(AssetStyle value);
[[nodiscard]] std::string to_string(QualityLevel value);
[[nodiscard]] std::string to_string(AntiAliasing value);
[[nodiscard]] std::string to_string(ToneMapping value);
[[nodiscard]] std::string to_string(ColorFilter value);

} // namespace openminerz
