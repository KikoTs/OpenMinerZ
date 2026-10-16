// Field of view: the projection arithmetic behind the "Field of view" setting
// (include/openminerz/view_projection.hpp), and the promises it makes -
// hands and held item scale together, zoom keeps retail's ratio, a bolt
// leaves the muzzle the player sees, and nothing gameplay reads the setting.

#include "openminerz/inventory.hpp"
#include "openminerz/player_nametag.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"
#include "openminerz/view_projection.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/shadow_cascade.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace openminerz;

namespace
{

void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double left, double right, double tolerance)
{
    return std::abs(left - right) <= tolerance;
}

constexpr double pi = 3.14159265358979323846;

double radians(double degrees)
{
    return degrees * pi / 180.0;
}

// The reference the header is checked against, in double precision.
double horizontal_reference(double vertical_degrees, double aspect)
{
    return 2.0 * std::atan(std::tan(radians(vertical_degrees) * 0.5) * aspect) * 180.0 / pi;
}

constexpr std::array<float, 5> aspects{4.0F / 3.0F, 16.0F / 10.0F, 16.0F / 9.0F, 21.0F / 9.0F,
                                       32.0F / 9.0F};
constexpr std::array<float, 7> settings_sweep{36.0F, 50.0F, 59.0F, 73.0F, 74.0F, 90.0F, 105.0F};
constexpr std::array<float, 6> magnifications{1.0F, 1.15F, 1.2F, 2.0F, 3.45F, 8.0F};

void the_setting_is_vertical_and_hor_plus()
{
    // Retail's 73 degrees, measured side to side on the common displays.
    check(near(view::horizontal_fov_degrees(73.0F, 16.0F / 9.0F), 105.52, 0.01),
          "73 degrees vertical is 105.5 degrees across 16:9");
    check(near(view::horizontal_fov_degrees(73.0F, 4.0F / 3.0F), 89.23, 0.01),
          "73 degrees vertical is 89.2 degrees across 4:3");
    check(near(view::horizontal_fov_degrees(73.0F, 21.0F / 9.0F), 119.84, 0.01),
          "73 degrees vertical is 119.8 degrees across 21:9");
    for (const float aspect : aspects)
    {
        float previous = 0.0F;
        for (const float setting : settings_sweep)
        {
            const float horizontal = view::horizontal_fov_degrees(setting, aspect);
            check(near(horizontal, horizontal_reference(setting, aspect), 0.002),
                  "horizontal = 2 atan(tan(vertical / 2) * aspect)");
            check(near(view::vertical_fov_degrees(horizontal, aspect), setting, 0.002),
                  "the conversion round-trips");
            check(horizontal > previous, "a larger setting is wider on every display");
            previous = horizontal;
        }
    }
    // Vertical-locked: the same setting shows the same height everywhere and a
    // wider display only ever adds to the sides.
    for (const float setting : settings_sweep)
        for (std::size_t index = 1; index < aspects.size(); ++index)
            check(view::horizontal_fov_degrees(setting, aspects[index]) >
                      view::horizontal_fov_degrees(setting, aspects[index - 1U]),
                  "a wider display sees more, never less");
    check(near(view::horizontal_fov_degrees(74.0F, 1.0F), 74.0, 0.001),
          "on a square display both angles are the setting");
}

void the_slider_range_is_sixty_to_one_thirty_three_across_sixteen_by_nine()
{
    check(view::minimum_vertical_fov_degrees == 36.0F && view::maximum_vertical_fov_degrees == 105.0F,
          "the stored range is 36..105 vertical degrees");
    check(view::reference_horizontal_degrees(view::minimum_vertical_fov_degrees) == 60,
          "the narrow end is 60 degrees across a 16:9 display");
    check(view::reference_horizontal_degrees(view::maximum_vertical_fov_degrees) == 133,
          "the wide end is 133 degrees across a 16:9 display");
    check(view::reference_horizontal_degrees(view::default_vertical_fov_degrees) == 107,
          "the default is 107 degrees across a 16:9 display");
    check(near(view::setting_normalized(view::minimum_vertical_fov_degrees), 0.0, 1e-6) &&
              near(view::setting_normalized(view::maximum_vertical_fov_degrees), 1.0, 1e-6) &&
              near(view::setting_normalized(70.5F), 0.5, 1e-6),
          "the slider is linear in vertical degrees");
    check(view::setting_normalized(-40.0F) == 0.0F && view::setting_normalized(900.0F) == 1.0F,
          "the slider position is clamped");
}

void an_unzoomed_frame_draws_exactly_the_setting()
{
    for (const float setting : settings_sweep)
    {
        for (const float magnification : magnifications)
            check(view::presented_vertical_fov_degrees(setting, magnification, 0.0F) == setting,
                  "progress 0 presents the setting itself, bit for bit");
        check(view::presented_vertical_fov_degrees(setting, 1.0F, 1.0F) == setting,
              "a tool has no zoom");
    }
    // The stored setting is range-checked by Settings::normalize(); whatever is
    // handed to a projection is at least an angle a projection can draw.
    check(view::presented_vertical_fov_degrees(-5.0F, 1.0F, 0.0F) ==
                  view::minimum_presented_fov_degrees &&
              view::presented_vertical_fov_degrees(400.0F, 1.0F, 0.0F) ==
                  view::maximum_presented_fov_degrees &&
              view::presented_vertical_fov_degrees(std::nanf(""), 1.0F, 0.0F) ==
                  view::default_vertical_fov_degrees,
          "a degenerate angle can never reach a projection");
    Settings stored;
    stored.vertical_fov = 5.0F;
    stored.normalize();
    check(stored.vertical_fov == view::minimum_vertical_fov_degrees,
          "a stored setting below the slider's range is brought into it");
    stored.vertical_fov = 400.0F;
    stored.normalize();
    check(stored.vertical_fov == view::maximum_vertical_fov_degrees,
          "and one above it");
}

void shouldering_reproduces_retail_at_retails_field_of_view()
{
    // Player.UpdateAnimation: Angle.Lerp(DefaultFOV, DefaultFOV / magnification, progress).
    for (const float magnification : magnifications)
        for (const float progress : {0.0F, 0.1F, 0.25F, 0.5F, 0.75F, 0.999F, 1.0F})
        {
            const float retail = std::lerp(73.0F, 73.0F / magnification, progress);
            check(near(view::retail_zoom_fov_degrees(magnification, progress), retail, 1e-4),
                  "retail's shouldered angle");
            check(near(view::presented_vertical_fov_degrees(73.0F, magnification, progress), retail,
                       2e-3),
                  "at 73 degrees the zoomed camera is retail's, at every point of the clip");
        }
    check(near(view::presented_vertical_fov_degrees(73.0F, 2.0F, 1.0F), 36.5, 2e-3),
          "a 2x gun fully shouldered at 73 degrees is 36.5 degrees");
}

void zoom_magnifies_by_the_same_factor_at_every_setting()
{
    for (const float magnification : magnifications)
        for (const float progress : {0.2F, 0.5F, 1.0F})
        {
            const double retail_ratio =
                std::tan(radians(36.5)) /
                std::tan(radians(std::lerp(73.0, 73.0 / static_cast<double>(magnification),
                                           static_cast<double>(progress))) *
                         0.5);
            check(near(view::zoom_ratio(magnification, progress), retail_ratio,
                       retail_ratio * 2e-5),
                  "zoom_ratio is retail's on-screen magnification");
            float previous = 0.0F;
            for (const float setting : settings_sweep)
            {
                const float zoomed =
                    view::presented_vertical_fov_degrees(setting, magnification, progress);
                const double ratio = std::tan(radians(setting) * 0.5) /
                                     std::tan(radians(static_cast<double>(zoomed)) * 0.5);
                check(near(ratio, retail_ratio, retail_ratio * 2e-4),
                      "aiming enlarges the picture by retail's factor whatever the setting");
                check(zoomed <= setting + 1e-4F, "zoom never widens the view");
                check(zoomed > previous, "a wider setting is still wider while aiming");
                check(zoomed >= view::minimum_presented_fov_degrees &&
                          zoomed <= view::maximum_presented_fov_degrees,
                      "a zoomed angle is one every pass can draw");
                previous = zoomed;
            }
        }
    // The clamp that used to sit in every pass (50 degrees) cut a 2x zoom short.
    check(view::presented_vertical_fov_degrees(74.0F, 2.0F, 1.0F) < 40.0F,
          "a 2x gun must zoom past the old 50 degree floor");
    // Progress moves the view continuously (no pop at either end).
    float previous = view::presented_vertical_fov_degrees(90.0F, 2.0F, 0.0F);
    for (int step = 1; step <= 100; ++step)
    {
        const float now =
            view::presented_vertical_fov_degrees(90.0F, 2.0F, static_cast<float>(step) / 100.0F);
        check(now <= previous + 1e-4F && previous - now < 1.5F, "the zoom is smooth and monotonic");
        previous = now;
    }
}

void the_viewmodel_keeps_retails_projection()
{
    check(view::viewmodel_fov_degrees(view::retail_viewmodel_fov_degrees, 0.0F) == 90.0F,
          "Avatar.EyePointCamera is 90 degrees");
    check(view::viewmodel_fov_degrees(view::retail_viewmodel_fov_degrees, 1.0F) == 45.0F,
          "ShoulderedAvatarFOV is 45 degrees");
    check(near(view::viewmodel_fov_degrees(90.0F, 0.5F), 67.5, 1e-4),
          "retail lerps the avatar camera's angle");
    check(view::viewmodel_fov_degrees(10.0F, 0.0F) == view::minimum_viewmodel_fov_degrees &&
              view::viewmodel_fov_degrees(500.0F, 0.0F) == view::maximum_viewmodel_fov_degrees &&
              view::viewmodel_fov_degrees(std::nanf(""), 0.0F) == 90.0F,
          "the advanced value stays inside its range");
    // FOV changes scale hands and weapon together, preserving the default
    // framing and the same ADS magnification at every setting.
    Settings settings;
    check(settings.viewmodel_fov == view::retail_viewmodel_fov_degrees,
          "the hands default to retail's 90 degrees");
    for (const float world_setting : settings_sweep)
    {
        settings.vertical_fov = world_setting;
        settings.normalize();
        for (const float progress : {0.0F, 0.25F, 0.5F, 1.0F})
        {
            const float base = view::viewmodel_fov_degrees(settings.viewmodel_fov, progress);
            const float shown = view::presented_viewmodel_fov_degrees(
                settings.viewmodel_fov, world_setting, progress);
            const double expected_scale = std::tan(radians(74.0F) * 0.5) /
                                          std::tan(radians(world_setting) * 0.5);
            const view::ViewPoint hand{0.12F, -0.10F, 0.5F};
            const view::ViewPoint weapon{0.08F, -0.06F, 0.7F};
            for (const auto point : {hand, weapon})
            {
                const auto reference = view::view_to_ndc(point, base, view::reference_aspect);
                const auto scaled = view::view_to_ndc(point, shown, view::reference_aspect);
                check(near(scaled[0], reference[0] * expected_scale, 2e-5) &&
                          near(scaled[1], reference[1] * expected_scale, 2e-5),
                      "hands and weapon must scale by the same FOV perspective ratio");
            }
        }
    }
    check(view::presented_viewmodel_fov_degrees(90.0F, 105.0F, 0.0F) > 110.0F,
          "wide FOV must not hit the old viewmodel projection clamp");
}

void a_bolt_is_drawn_from_the_muzzle_the_player_sees()
{
    // Muzzle positions in the viewmodel camera: hip, shouldered, far right.
    constexpr std::array<view::ViewPoint, 4> muzzles{{{0.14F, -0.11F, 0.62F},
                                                      {0.0F, -0.035F, 0.48F},
                                                      {0.31F, -0.20F, 0.90F},
                                                      {-0.08F, 0.02F, 0.30F}}};
    for (const float aspect : aspects)
        for (const float world_setting : settings_sweep)
            for (const float progress : {0.0F, 0.4F, 1.0F})
            {
                const float viewmodel_fov =
                    view::presented_viewmodel_fov_degrees(90.0F, world_setting, progress);
                const float world_fov =
                    view::presented_vertical_fov_degrees(world_setting, 2.0F, progress);
                for (const view::ViewPoint &muzzle : muzzles)
                {
                    const view::ViewPoint world =
                        view::viewmodel_to_world_view(muzzle, viewmodel_fov, world_fov);
                    const auto seen = view::view_to_ndc(muzzle, viewmodel_fov, aspect);
                    const auto drawn = view::view_to_ndc(world, world_fov, aspect);
                    check(near(seen[0], drawn[0], 2e-5) && near(seen[1], drawn[1], 2e-5),
                          "the reconciled point covers the muzzle's own pixel");
                    check(world.z == muzzle.z, "and sits at the muzzle's distance");
                }
            }
    // Equal angles need no correction; retail's uncorrected reuse is off by
    // tan(45) / tan(36.5) = 1.351 at its own 73/90 pair.
    const view::ViewPoint same = view::viewmodel_to_world_view({0.2F, -0.1F, 0.6F}, 90.0F, 90.0F);
    check(near(same.x, 0.2, 1e-6) && near(same.y, -0.1, 1e-6), "identical cameras agree");
    const view::ViewPoint muzzle{0.14F, -0.11F, 0.62F};
    const auto seen = view::view_to_ndc(muzzle, 90.0F, 16.0F / 9.0F);
    const auto unreconciled = view::view_to_ndc(muzzle, 73.0F, 16.0F / 9.0F);
    check(near(unreconciled[0] / seen[0], 1.3514, 1e-3),
          "without reconciliation a bolt starts 1.35x further out than the gun");

    // The seen start is worn off as the bolt flies, so it arrives on its true path.
    check(view::muzzle_blend(0.0F, 12.0F) == 1.0F, "a bolt starts on the visible muzzle");
    check(view::muzzle_blend(12.0F, 12.0F) == 0.0F && view::muzzle_blend(80.0F, 12.0F) == 0.0F,
          "and is on its simulated path after the blend distance");
    check(view::muzzle_blend(5.0F, 0.0F) == 0.0F, "no blend distance means no offset");
    float previous = 1.0F;
    for (int step = 1; step <= 48; ++step)
    {
        const float now = view::muzzle_blend(static_cast<float>(step) * 0.25F, 12.0F);
        check(now <= previous && now >= 0.0F, "the offset only ever shrinks");
        previous = now;
    }
}

void gameplay_never_reads_the_setting()
{
    // The guided launcher's lock cone: 0.3 of retail's FPS camera, shouldered
    // or not. The function has no way to see the player's setting.
    check(near(view::retail_lock_cone_radians(2.0F, 0.0F), radians(73.0) * 0.3, 1e-6),
          "the lock cone is 0.3 of 73 degrees at the hip");
    check(near(view::retail_lock_cone_radians(2.0F, 1.0F), radians(36.5) * 0.3, 1e-6),
          "and 0.3 of 36.5 degrees through the guided launcher's 2x sight");
    check(view::retail_vertical_fov_degrees == world::PlayerSimulation::gameplay_fov_degrees,
          "gameplay's camera is retail's 73 degrees");
    // Reach is a simulation constant, not a projection.
    check(world::PlayerSimulation::interaction_reach == 5.0F, "reach does not follow the view");
    // Look input is an angle per count; retail's shouldered factor is fixed.
    check(view::look_scale(false) == 1.0F && view::look_scale(true) == 0.25F,
          "Player.ControlSensitivity is 1, or 0.25 in every shouldering state");
}

void the_reticle_brackets_the_same_cone_at_every_setting()
{
    constexpr float half_degree = 0.5F * view::radians_per_degree;
    // InGameHUD: spread / FPSCamera.FieldOfView (tests/test_hud_layout.cpp pins
    // 0.5 / 73 = 0.00684931455 for a tool).
    check(near(view::reticle_fraction(half_degree, view::default_vertical_fov_degrees),
               0.00684931455, 1e-8),
          "at the default setting the reticle is retail's, pixel for pixel");
    const GunStats *pistol = gun_stats(InventoryItemId::pistol);
    check(pistol != nullptr, "the pistol has stats");
    check(near(view::reticle_fraction(pistol->inaccuracy, view::default_vertical_fov_degrees),
               pistol->inaccuracy / radians(73.0), 1e-7),
          "a gun's spread over retail's 73 degrees");
    for (const float setting : settings_sweep)
    {
        const double scale = std::tan(radians(view::default_vertical_fov_degrees) * 0.5) /
                             std::tan(radians(setting) * 0.5);
        check(near(view::reticle_fraction(half_degree, setting), 0.00684931455 * scale, 1e-7),
              "the opening follows the picture's scale");
    }
    check(view::reticle_fraction(half_degree, 105.0F) < view::reticle_fraction(half_degree, 74.0F) &&
              view::reticle_fraction(half_degree, 50.0F) > view::reticle_fraction(half_degree, 74.0F),
          "a wider view draws the same spread smaller");
}

void nametags_follow_the_camera_that_drew_the_frame()
{
    // GameScreen.gameScreen_AfterDraw projects with the main view's own
    // projection, so a tag stays over its player at every field of view.
    const world::Vec3 feet{6.0F, 30.0F, -14.0F};
    for (const float aspect : aspects)
        for (const float fov : {9.4F, 36.0F, 73.0F, 105.0F})
        {
            const float height = 900.0F;
            const float width = height * aspect;
            const NametagCamera camera{{0.0F, 31.4F, 0.0F}, 0.3F, 0.05F, fov, width, height, true};
            const auto anchor = project_nametag(camera, feet);
            check(anchor.has_value(), "a player ahead has an anchor");
            // The same point through the shared view arithmetic.
            const float dx = feet.x - camera.eye.x;
            const float dy = feet.y + nametag_height_above_feet - camera.eye.y;
            const float dz = feet.z - camera.eye.z;
            const float right = dx * std::cos(camera.yaw) + dz * std::sin(camera.yaw);
            const float level = dx * std::sin(camera.yaw) - dz * std::cos(camera.yaw);
            const float up = dy * std::cos(camera.pitch) - level * std::sin(camera.pitch);
            const float depth = level * std::cos(camera.pitch) + dy * std::sin(camera.pitch);
            const auto ndc = view::view_to_ndc({right, up, depth}, fov, aspect);
            check(near(anchor->x, width * 0.5F * (1.0F + ndc[0]), 0.02) &&
                      near(anchor->y, height * 0.5F * (1.0F - ndc[1]), 0.02),
                  "the tag is projected with the frame's own field of view");
        }
}

void shadow_cascades_contain_the_widest_and_the_narrowest_frustum()
{
    // The cascades are fitted to the camera's own tangents, so the wider (and
    // the zoomed) frustum is covered without a second code path.
    for (const float aspect : aspects)
        for (const float fov : {9.4F, 21.0F, 36.0F, 74.0F, 105.0F})
        {
            const float ty = view::half_tangent(fov);
            const float tx = ty * aspect;
            float previous = 0.06F;
            float start = 0.06F;
            for (const float split : {16.0F, 48.0F, 150.0F})
            {
                const auto slice = world::shadow_slice(previous, start, split, tx, ty);
                for (const float depth : {slice.near_depth, slice.far_depth})
                    check(depth * depth * (tx * tx + ty * ty) +
                                  (depth - slice.center_depth) * (depth - slice.center_depth) <=
                              slice.radius * slice.radius * 1.0001F + 0.001F,
                          "the cascade sphere holds every corner of the frustum slice");
                previous = start;
                start = split;
            }
        }
}

struct TemporaryFile final
{
    std::filesystem::path path;
    explicit TemporaryFile(std::string_view name)
        : path(std::filesystem::temp_directory_path() /
               ("openminerz-fov-" + std::string{name} + ".cfg"))
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    ~TemporaryFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    void write(std::string_view text) const
    {
        std::ofstream output{path, std::ios::trunc};
        output << text;
    }
};

void saved_settings_keep_the_view_they_had()
{
    const Settings defaults;
    check(defaults.vertical_fov == view::default_vertical_fov_degrees,
          "the default field of view is the one the game has always opened with");
    check(defaults.viewmodel_fov == view::retail_viewmodel_fov_degrees,
          "the hands default to retail's projection");

    // Every whole value the previous slider could store (50..105) loads as itself.
    for (int stored = 50; stored <= 105; ++stored)
    {
        const TemporaryFile file{"legacy"};
        file.write("vertical_fov=" + std::to_string(stored) + "\nviewmodel_fov=90\n");
        const Settings loaded = SettingsStore{file.path}.load();
        check(loaded.vertical_fov == static_cast<float>(stored),
              "a saved field of view is drawn exactly as before");
        check(loaded.viewmodel_fov == 90.0F, "and the hands with it");
    }
    {
        // A file that predates the key, or has neither key, opens on the default.
        const TemporaryFile file{"absent"};
        file.write("vsync=1\n");
        const Settings loaded = SettingsStore{file.path}.load();
        check(loaded.vertical_fov == defaults.vertical_fov &&
                  loaded.viewmodel_fov == defaults.viewmodel_fov,
              "a file without the keys shows today's view");
    }
    {
        // A hands value someone tuned with the old second slider still applies.
        const TemporaryFile file{"hands"};
        file.write("vertical_fov=96\nviewmodel_fov=78\n");
        const Settings loaded = SettingsStore{file.path}.load();
        check(loaded.vertical_fov == 96.0F && loaded.viewmodel_fov == 78.0F,
              "a tuned held-item value survives as the advanced option it now is");
        SettingsStore{file.path}.save(loaded);
        const Settings again = SettingsStore{file.path}.load();
        check(again.vertical_fov == 96.0F && again.viewmodel_fov == 78.0F,
              "and round-trips through a save");
    }
    {
        const TemporaryFile file{"range"};
        file.write("vertical_fov=20\n");
        check(SettingsStore{file.path}.load().vertical_fov == view::minimum_vertical_fov_degrees,
              "too narrow clamps to the slider's minimum");
        file.write("vertical_fov=170\n");
        check(SettingsStore{file.path}.load().vertical_fov == view::maximum_vertical_fov_degrees,
              "too wide clamps to the slider's maximum");
        file.write("vertical_fov=nan\n");
        check(SettingsStore{file.path}.load().vertical_fov == defaults.vertical_fov,
              "a value that is not a number is ignored");
    }
}

void the_menu_has_one_field_of_view_slider()
{
    int fov_rows = 0;
    const ui::SettingRow *row = nullptr;
    for (const ui::SettingsTab tab : ui::all_tabs())
        for (const ui::SettingRow &candidate : ui::rows_for_tab(tab))
        {
            check(candidate.id != ui::SettingId::viewmodel_fov,
                  "the held-item angle is not a menu row: the hands must not change");
            if (candidate.id == ui::SettingId::vertical_fov)
            {
                ++fov_rows;
                row = &candidate;
                check(tab == ui::SettingsTab::display, "the slider lives on the Display page");
            }
        }
    check(fov_rows == 1 && row != nullptr, "exactly one field of view slider");
    check(row->label == "Field of view" && row->kind == ui::SettingKind::slider,
          "it is called Field of view and it is a slider");
    check(!row->requires_restart, "it applies at once (live preview)");
    check(row->description.size() <= 170U, "the description fits the menu's two lines");
    check(row->description.find("top to bottom") != std::string_view::npos,
          "the description says the number is the vertical angle");
    check(row->description.find("hands") != std::string_view::npos,
          "and that the hands do not change");

    Settings settings;
    check(ui::setting_value(settings, ui::SettingId::vertical_fov) == "74 DEGREES",
          "the read-out is the vertical angle in whole degrees");
    check(near(ui::setting_normalized_value(settings, ui::SettingId::vertical_fov),
               view::setting_normalized(74.0F), 1e-6),
          "the bar shows where 74 sits in 36..105");
    ui::adjust_setting(settings, ui::SettingId::vertical_fov, 1);
    check(settings.vertical_fov == 75.0F, "one step is one degree");
    ui::adjust_setting(settings, ui::SettingId::vertical_fov, -1);
    ui::adjust_setting(settings, ui::SettingId::vertical_fov, -1);
    check(settings.vertical_fov == 73.0F, "down to retail's 73");
    for (int step = 0; step < 200; ++step)
        ui::adjust_setting(settings, ui::SettingId::vertical_fov, -1);
    check(settings.vertical_fov == view::minimum_vertical_fov_degrees &&
              ui::setting_value(settings, ui::SettingId::vertical_fov) == "36 DEGREES" &&
              ui::setting_normalized_value(settings, ui::SettingId::vertical_fov) == 0.0F,
          "the slider stops at its narrow end");
    for (int step = 0; step < 200; ++step)
        ui::adjust_setting(settings, ui::SettingId::vertical_fov, 1);
    check(settings.vertical_fov == view::maximum_vertical_fov_degrees &&
              ui::setting_value(settings, ui::SettingId::vertical_fov) == "105 DEGREES" &&
              ui::setting_normalized_value(settings, ui::SettingId::vertical_fov) == 1.0F,
          "and at its wide end");
    // A hand-edited fraction snaps to whole degrees on the first step.
    settings.vertical_fov = 80.4F;
    ui::adjust_setting(settings, ui::SettingId::vertical_fov, 1);
    check(settings.vertical_fov == 81.0F, "stepping lands on whole degrees");

    // The slider never touches the hands.
    check(settings.viewmodel_fov == view::retail_viewmodel_fov_degrees,
          "moving the slider leaves the hands' projection alone");

    // Resetting the page restores both, including the hidden advanced value.
    settings.vertical_fov = 101.0F;
    settings.viewmodel_fov = 70.0F;
    ui::reset_setting(settings, ui::SettingId::vertical_fov);
    check(settings.vertical_fov == view::default_vertical_fov_degrees &&
              settings.viewmodel_fov == 70.0F,
          "resetting the row resets the world's angle only");
    settings.vertical_fov = 101.0F;
    ui::reset_tab(settings, ui::SettingsTab::display);
    check(settings.vertical_fov == view::default_vertical_fov_degrees &&
              settings.viewmodel_fov == view::retail_viewmodel_fov_degrees,
          "resetting the Display page also returns the hands to retail");
}

} // namespace

int main()
{
    try
    {
        the_setting_is_vertical_and_hor_plus();
        // Regress the smoke near-plane failure over a complete walk through a
        // sphere: even after its centre passes behind us, forward density remains.
        for (float z = 0.9F; z > -0.95F; z -= 0.05F)
        {
            const auto bounds = view::volume_bounds(0, 0, z, 1, 74, 16.0F/9.0F);
            check(bounds.visible && bounds.left == -1 && bounds.right == 1 &&
                      bounds.bottom == -1 && bounds.top == 1,
                  "inside-volume rays must cover the viewport without crossing the near clip plane");
        }
        check(!view::volume_bounds(0, 0, -2, 1, 74, 16.0F/9.0F).visible,
              "a volume wholly behind the eye must not draw");
        const auto distant = view::volume_bounds(0, 0, 20, 1, 74, 16.0F/9.0F);
        check(distant.visible && distant.right-distant.left < 0.2F && distant.top-distant.bottom < 0.2F,
              "distant smoke must use bounded coverage instead of full-screen overdraw");
        the_slider_range_is_sixty_to_one_thirty_three_across_sixteen_by_nine();
        an_unzoomed_frame_draws_exactly_the_setting();
        shouldering_reproduces_retail_at_retails_field_of_view();
        zoom_magnifies_by_the_same_factor_at_every_setting();
        the_viewmodel_keeps_retails_projection();
        a_bolt_is_drawn_from_the_muzzle_the_player_sees();
        gameplay_never_reads_the_setting();
        the_reticle_brackets_the_same_cone_at_every_setting();
        nametags_follow_the_camera_that_drew_the_frame();
        shadow_cascades_contain_the_widest_and_the_narrowest_frustum();
        saved_settings_keep_the_view_they_had();
        the_menu_has_one_field_of_view_slider();
    }
    catch (const std::exception &error)
    {
        std::cerr << "view projection test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "view projection tests passed\n";
    return 0;
}
