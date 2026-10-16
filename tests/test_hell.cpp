// The Enhanced presentation's deep underground: the arithmetic of its light,
// air and exposure (openminerz/world/hell_atmosphere.hpp) and the lava map the
// shaders read (openminerz/world/lava_field.hpp). The shaders evaluate these
// same expressions with these same constants, so this needs no graphics device.

#include "openminerz/post_math.hpp"
#include "openminerz/world/hell_atmosphere.hpp"
#include "openminerz/world/lava_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace openminerz;
using namespace openminerz::world;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(float left, float right, float tolerance = 0.0005F)
{
    return std::abs(left - right) <= tolerance;
}

void the_deep_layer_arrives_with_depth_not_at_a_line()
{
    // The eye's own transition is the one the sky, the ambience and the older
    // fill always used; it must not have moved.
    check(near(hell_presence(-24.0F), 0.0F) && near(hell_presence(-30.0F), 0.5F) &&
              near(hell_presence(-36.0F), 1.0F),
          "the eye's transition band must stay -24 .. -36");
    // Surfaces: nothing in the caves above, everything at Hell's roof.
    check(near(hell_surface_presence(0.0F), 0.0F) && near(hell_surface_presence(-20.0F), 0.0F) &&
              near(hell_surface_presence(hell_surface_fade_top), 0.0F),
          "an ordinary cave must take none of the deep layer's glow");
    check(near(hell_surface_presence(hell_surface_fade_bottom), 1.0F) &&
              near(hell_surface_presence(-60.0F), 1.0F),
          "Hell's roof and everything under it must take all of it");
    check(hell_surface_fade_top < -24.0F && hell_surface_fade_bottom >= -36.0F,
          "surfaces must fade in inside the eye's band, below its start");
    float previous = hell_surface_presence(-20.0F);
    for (float height = -20.0F; height >= -45.0F; height -= 0.05F)
    {
        const float presence = hell_surface_presence(height);
        check(presence >= previous - 1.0e-6F, "deeper must never be less lit by the deep layer");
        check(presence - previous < 0.012F, "the deep layer must fade in, not switch on");
        check(presence >= 0.0F && presence <= 1.0F, "presence is a share");
        previous = presence;
    }
}

void exposure_opens_only_in_hell_and_still_closes_over_lava()
{
    // Above ground and in caves it is exactly the Enhanced rule: it may close
    // down, never open. A night or a cave is never brightened into day.
    for (float luminance = 0.0001F; luminance < 60.0F; luminance *= 1.13F)
        check(near(hell_auto_exposure(luminance, 0.0F), post::auto_exposure(luminance), 1.0e-6F),
              "outside Hell the exposure rule must not change");
    check(near(hell_exposure_limit(0.0F), 1.0F) && near(hell_exposure_limit(-3.0F), 1.0F),
          "no headroom above Hell");
    check(near(hell_exposure_limit(1.0F), hell_exposure_headroom) &&
              near(hell_exposure_limit(7.0F), hell_exposure_headroom),
          "headroom is bounded");
    check(hell_exposure_headroom > 1.3F && hell_exposure_headroom <= 2.83F,
          "Hell may open by up to a stop and a half, no more: lava must not burn out");
    // Dark rock with no lava in view: the eye opens fully.
    check(near(hell_auto_exposure(0.03F, 1.0F), hell_exposure_headroom),
          "dark rock in Hell must be opened up, not crushed to black");
    // A view filled by a lava lake closes down, like bright snow does.
    check(hell_auto_exposure(0.40F, 1.0F) < 0.55F && hell_auto_exposure(5.0F, 1.0F) < 0.36F,
          "a lava lake must close the exposure");
    for (const float presence : {0.0F, 0.3F, 0.7F, 1.0F})
    {
        float previous = hell_auto_exposure(0.0005F, presence);
        for (float luminance = 0.0006F; luminance < 40.0F; luminance *= 1.07F)
        {
            const float exposure = hell_auto_exposure(luminance, presence);
            check(exposure <= previous + 1.0e-6F, "a brighter view never raises the exposure");
            check(exposure >= hell_exposure_floor - 1.0e-6F, "exposure has a floor");
            previous = exposure;
        }
    }
    // Walking down: the limit rises with depth, smoothly.
    float previous = hell_exposure_limit(0.0F);
    for (float presence = 0.0F; presence <= 1.0F; presence += 0.01F)
    {
        const float limit = hell_exposure_limit(presence);
        check(limit >= previous - 1.0e-6F &&
                  limit - previous < (hell_exposure_headroom - 1.0F) * 0.0101F,
              "the exposure limit must follow the descent without a step");
        previous = limit;
    }
}

void lava_lights_what_faces_it_and_fades_with_height()
{
    check(near(lava_form_factor(-1.0F), 1.0F) && near(lava_form_factor(0.0F), 0.5F) &&
              near(lava_form_factor(1.0F), 0.0F),
          "a roof sees the whole lake, a wall half, a floor none");
    // Under the lava, nothing. Right above it the voxel light field owns the
    // light; the area light only takes over higher up.
    check(near(lava_height_falloff(-3.0F), 0.0F) && near(lava_height_falloff(0.0F), 0.0F) &&
              near(lava_height_falloff(lava_far_fade_in_start), 0.0F),
          "the area light must not double the flood-filled light at the lava");
    check(lava_height_falloff(lava_far_fade_in_end) > 0.8F, "it must have taken over by then");
    check(near(lava_height_falloff(lava_far_half_height), 0.5F, 0.001F),
          "half the light at the half height");
    float previous = lava_height_falloff(lava_far_fade_in_end);
    for (float height = lava_far_fade_in_end; height < 80.0F; height += 0.25F)
    {
        const float falloff = lava_height_falloff(height);
        check(falloff <= previous + 1.0e-6F && falloff > 0.0F,
              "above the take-over the light only fades");
        previous = falloff;
    }
    check(lava_height_falloff(64.0F) < 0.10F,
          "the lake under Hell must not light the caves far above it");
    // The footprint read from the map widens with height and stays in range.
    check(near(lava_footprint_level(0.0F), 0.0F) && near(lava_footprint_level(-5.0F), 0.0F),
          "at the lava the footprint is one block");
    previous = 0.0F;
    for (float height = 0.0F; height < 200.0F; height += 0.5F)
    {
        const float level = lava_footprint_level(height);
        check(level >= previous - 1.0e-6F && level <= lava_far_top_level + 1.0e-6F,
              "a higher surface reads a wider footprint, within the map's levels");
        previous = level;
    }
    check(lava_far_top_level < static_cast<float>(lava_field_levels - 1),
          "the widest footprint must be a level the map has");
    // The whole term: bounded, zero for a floor, zero without lava.
    for (float height = -4.0F; height < 60.0F; height += 1.0F)
        for (float normal_y = -1.0F; normal_y <= 1.0F; normal_y += 0.25F)
            for (const float coverage : {0.0F, 0.2F, 1.0F, 3.0F})
            {
                const float light = lava_far_light(coverage, height, normal_y);
                check(light >= 0.0F && light <= 1.0F, "lava light is a share of the lava's own");
                if (coverage <= 0.0F || normal_y >= 1.0F)
                    check(near(light, 0.0F), "no lava in view, no light from it");
            }
    check(lava_far_light(1.0F, 22.0F, -1.0F) > lava_far_light(1.0F, 22.0F, 0.0F) &&
              lava_far_light(1.0F, 22.0F, 0.0F) > lava_far_light(1.0F, 22.0F, 0.9F),
          "the roof over a lake is lit more than a wall beside it, a wall more than a floor");
    check(lava_far_light(0.25F, 22.0F, -1.0F) < lava_far_light(1.0F, 22.0F, -1.0F),
          "a small pool lights less than a lake");
}

void the_air_glows_and_shimmers_above_lava_only()
{
    check(near(lava_air_share(-2.0F, lava_glow_height), 0.0F) &&
              near(lava_air_share(-40.0F, lava_heat_height), 0.0F),
          "nothing rises from under the lava");
    check(near(lava_air_share(0.0F, lava_glow_height), 1.0F), "the air is hottest at the lava");
    check(near(lava_air_share(lava_glow_height, lava_glow_height), std::exp(-1.0F)),
          "the glow thins out by its scale height");
    check(lava_heat_height < lava_glow_height, "heat stays lower than light");
    check(lava_air_share(60.0F, lava_glow_height) < 0.001F,
          "the lake under Hell must not glow in the air of the surface above it");
    float previous = 1.0F;
    for (float height = 0.0F; height < 40.0F; height += 0.25F)
    {
        const float share = lava_air_share(height, lava_heat_height);
        check(share <= previous + 1.0e-6F && share >= 0.0F, "heat only thins with height");
        previous = share;
    }
    // The shimmer: none without heat, saturating, and next to nothing for
    // what the player is working on.
    check(near(heat_haze_amount(0.0F), 0.0F) && near(heat_haze_amount(-5.0F), 0.0F),
          "no hot air, no shimmer");
    check(heat_haze_amount(1.5F) < 0.13F, "the block in front of the player must hold still");
    check(heat_haze_amount(40.0F) > 0.95F && heat_haze_amount(400.0F) <= 1.0F,
          "a long look across a lake shimmers fully, and never more");
    previous = 0.0F;
    for (float path = 0.0F; path < 200.0F; path += 0.5F)
    {
        const float amount = heat_haze_amount(path);
        check(amount >= previous - 1.0e-6F && amount <= 1.0F, "more hot air never shimmers less");
        previous = amount;
    }
    // Never nauseating: the picture moves by under three pixels at 1080p and
    // under four at 1440p, a quarter of a percent of its height.
    check(heat_haze_reach * 1080.0F < 3.0F && heat_haze_reach * 1440.0F < 4.0F &&
              heat_haze_reach >= 0.0008F,
          "the shimmer must be visible and small");
}

void lava_breathes_slowly()
{
    double total{};
    float lowest = 10.0F;
    float highest{};
    float previous = lava_pulse(0.0F);
    constexpr float step = 1.0F / 240.0F;
    constexpr int samples = 240 * 240;
    for (int index = 1; index <= samples; ++index)
    {
        const float pulse = lava_pulse(static_cast<float>(index) * step);
        lowest = std::min(lowest, pulse);
        highest = std::max(highest, pulse);
        total += pulse;
        // Per second the light changes by well under a third of itself: a
        // swell, not a flicker.
        check(std::abs(pulse - previous) / step < 0.30F, "the pulse must be slow");
        previous = pulse;
    }
    check(lowest > 0.86F && highest < 1.14F, "the pulse must stay within 13% of steady");
    check(highest - lowest > 0.12F, "the pulse must be visible");
    check(std::abs(total / samples - 1.0) < 0.01, "the pulse must not brighten or dim the scene");
    check(near(lava_pulse(0.0F), lava_pulse(0.0F)), "the pulse is a pure function of the clock");
}

void the_palette_is_warm_but_not_one_red()
{
    const auto luminance = [](SkyColor color) {
        return 0.2126F * color.red + 0.7152F * color.green + 0.0722F * color.blue;
    };
    // Light from below is the stronger: it is what gives rock its shape.
    check(luminance(hell_glow_from_below) > luminance(hell_glow_from_above) * 1.4F,
          "an overhang must be lit more from below than a floor from above");
    for (const SkyColor color : {hell_glow_from_above, hell_glow_from_below, hell_lava_light})
    {
        check(color.red > color.green && color.green > color.blue, "the light must be warm");
        // Pure red is what made Hell read as dark: it carries a fifth of the
        // luminance of white. Enough green keeps texture and hue.
        check(color.green >= color.red * 0.25F, "the light must not be one flat red");
        check(color.green <= color.red * 0.50F, "the light must stay reddish, not brown");
    }
    check(luminance(hell_lava_light) > luminance(hell_glow_from_below) * 1.5F,
          "the lake must outshine the cavern's own glow, or it lights nothing");
    // The far haze is the cavern's dark: dimmer than the glow a floor gets.
    check(luminance(hell_far_haze) < luminance(hell_glow_from_above) * 0.25F,
          "the distance must be darker than the lit rock in front of it");
    check(hell_far_haze.red > hell_far_haze.green && hell_far_haze.green > hell_far_haze.blue,
          "the far haze must be warm");
}

void the_first_person_body_takes_the_same_light()
{
    const HellBodyLight nothing = hell_body_light(0.0F, 0.0F, 1.0F, 1.0F);
    check(near(nothing.ambient.red, 0.0F) && near(nothing.below.red, 0.0F) &&
              near(nothing.below.green, 0.0F),
          "above the deep layer the body takes none of its light");
    const HellBodyLight dry = hell_body_light(1.0F, 0.0F, 1.0F, 1.0F);
    check(near(dry.ambient.red, hell_glow_from_above.red) &&
              near(dry.ambient.green, hell_glow_from_above.green),
          "the body's fill is the glow a floor receives");
    check(near(dry.ambient.red + dry.below.red, hell_glow_from_below.red) &&
              near(dry.ambient.blue + dry.below.blue, hell_glow_from_below.blue),
          "a surface of the body turned down receives what an overhang does");
    const HellBodyLight lit = hell_body_light(1.0F, 0.5F, 1.0F, 1.0F);
    check(lit.below.red > dry.below.red + 0.3F && near(lit.ambient.red, dry.ambient.red),
          "lava lights the body from below, not from all round");
    check(near(lit.below.red - dry.below.red, hell_lava_light.red * 0.5F),
          "by the same share the rock beside it takes");
    const HellBodyLight strong = hell_body_light(1.0F, 0.5F, 2.0F, 1.0F);
    check(near(strong.ambient.red, dry.ambient.red * 2.0F) && strong.below.red > lit.below.red,
          "the Deep underground light setting scales it");
    const HellBodyLight half = hell_body_light(0.5F, 0.0F, 1.0F, 1.0F);
    check(near(half.ambient.red, dry.ambient.red * 0.5F), "it arrives with the depth");
}

std::vector<LavaColumn> lake(int first_x, int first_z, int side, int top)
{
    std::vector<LavaColumn> columns;
    for (int z = first_z; z < first_z + side; ++z)
        for (int x = first_x; x < first_x + side; ++x)
            columns.push_back({x, z, static_cast<std::int16_t>(top)});
    return columns;
}

// Coverage summed over one level, in blocks of lava.
double blocks_of_lava(const LavaField &field, int level, float LavaField::Texel::*layer)
{
    double total{};
    for (const LavaField::Texel &texel : field.level(level))
        total += static_cast<double>(texel.*layer);
    const double texel_side = static_cast<double>(1 << level);
    return total * texel_side * texel_side;
}

void the_lava_map_holds_what_the_meshes_show()
{
    LavaField field;
    check(field.empty() && field.take_dirty_slots().empty(), "a new map is empty and clean");
    check(near(field.sample(3.0F, 3.0F, 0.0F).deep, 0.0F), "an empty map shows no lava");

    // A whole chunk of the Hell lake, and a 4x4 pocket in a cave above it in
    // another chunk.
    field.set_section({0, 0, 0}, lake(0, 0, 16, -59));
    field.set_section({2, 1, 2}, lake(36, 20, 4, -20));
    check(!field.empty() && field.section_count() == 2U, "both sections are held");
    const std::vector<std::array<int, 2>> dirty = field.take_dirty_slots();
    check(dirty.size() == 2U && field.take_dirty_slots().empty(),
          "each changed chunk is reported once");

    // Level 0 is exact; every coarser level holds the same amount of lava.
    const LavaField::Sample in_lake = field.sample(8.0F, 8.0F, 0.0F);
    check(near(in_lake.deep, 1.0F) && near(in_lake.deep_top, -59.0F, 0.01F) &&
              near(in_lake.shallow, 0.0F),
          "the lake is on the deep layer at its own height");
    const LavaField::Sample in_pocket = field.sample(38.0F, 22.0F, 0.0F);
    check(near(in_pocket.shallow, 1.0F) && near(in_pocket.shallow_top, -20.0F, 0.01F) &&
              near(in_pocket.deep, 0.0F),
          "a cave's pocket is on the shallow layer at its own height");
    check(near(field.sample(100.0F, 100.0F, 0.0F).deep, 0.0F), "rock shows no lava");
    for (int level = 0; level < lava_field_levels; ++level)
    {
        check(std::abs(blocks_of_lava(field, level, &LavaField::Texel::deep) - 256.0) < 0.01,
              "every level must hold the same lake: light is spread, never added");
        check(std::abs(blocks_of_lava(field, level, &LavaField::Texel::shallow) - 16.0) < 0.01,
              "every level must hold the same pocket");
    }
    // A wide read still knows the height of what it averaged.
    const LavaField::Sample wide = field.sample(8.0F, 8.0F, 5.0F);
    check(wide.deep > 0.05F && wide.deep < 1.0F && near(wide.deep_top, -59.0F, 0.05F),
          "a wide footprint sees part lake, at the lake's height");
    const LavaField::Sample wide_pocket = field.sample(38.0F, 22.0F, 4.0F);
    check(wide_pocket.shallow > 0.0F && wide_pocket.shallow < 0.2F &&
              near(wide_pocket.shallow_top, -20.0F, 0.05F),
          "a lone pocket is a small share of a wide footprint: it fades with distance");

    // Two sections of one chunk: a column keeps its highest surface per layer.
    field.set_section({0, 0, 1}, {{4, 4, -50}, {5, 5, -16}});
    const LavaField::Sample stacked = field.sample(4.5F, 4.5F, 0.0F);
    check(near(stacked.deep, 1.0F) && near(stacked.deep_top, -50.0F, 0.01F),
          "the higher of two deep surfaces in a column wins");
    const LavaField::Sample both = field.sample(5.5F, 5.5F, 0.0F);
    check(near(both.deep, 1.0F) && near(both.deep_top, -59.0F, 0.01F) &&
              near(both.shallow, 1.0F) && near(both.shallow_top, -16.0F, 0.01F),
          "a pocket over the lake does not hide the lake");
    // Setting the same columns again changes nothing and uploads nothing.
    (void)field.take_dirty_slots();
    field.set_section({0, 0, 1}, {{4, 4, -50}, {5, 5, -16}});
    check(field.take_dirty_slots().empty(), "an unchanged section must not be uploaded again");
    // Removing a section removes its lava on the next read.
    field.set_section({0, 0, 1}, {});
    check(near(field.sample(4.5F, 4.5F, 0.0F).deep_top, -59.0F, 0.01F) &&
              near(field.sample(5.5F, 5.5F, 0.0F).shallow, 0.0F),
          "mined-out or covered lava must leave the map");
    field.set_section({2, 1, 2}, {});
    check(near(field.sample(38.0F, 22.0F, 0.0F).shallow, 0.0F) &&
              std::abs(blocks_of_lava(field, 6, &LavaField::Texel::shallow)) < 1.0e-6,
          "a removed pocket leaves no trace at any level");
    // Columns that are not in the section's own chunk are ignored.
    field.set_section({5, 5, 0}, {{0, 0, -59}, {85, 86, -59}});
    check(near(field.sample(85.5F, 86.5F, 0.0F).deep, 1.0F) &&
              std::abs(blocks_of_lava(field, 3, &LavaField::Texel::deep) - 257.0) < 0.01,
          "a section only contributes columns of its own chunk");
}

void the_lava_map_follows_the_player_without_wrapping()
{
    LavaField field;
    field.set_section({0, 0, 0}, lake(0, 0, 16, -59));
    // A chunk far outside the window is remembered but not shown where it
    // would wrap round to.
    field.set_section({16, 0, 0}, lake(256, 0, 16, -59));
    check(near(field.sample(8.0F, 8.0F, 0.0F).deep, 1.0F), "the near lake is in the map");
    check(std::abs(blocks_of_lava(field, 0, &LavaField::Texel::deep) - 256.0) < 0.01,
          "a lake sixteen chunks away must not alias onto the near one");
    (void)field.take_dirty_slots();

    // Walking a chunk at a time keeps what stays in the window untouched.
    field.recenter(1, 0);
    check(field.take_dirty_slots().empty(),
          "re-centring must redraw only chunks that entered or left");
    check(near(field.sample(8.0F, 8.0F, 0.0F).deep, 1.0F), "the lake is still there");

    // Far enough, the first lake leaves and the second takes its slot.
    field.recenter(16, 0);
    const std::vector<std::array<int, 2>> swapped = field.take_dirty_slots();
    check(swapped.size() == 1U && swapped.front()[0] == 0 && swapped.front()[1] == 0,
          "the one slot both lakes share is redrawn once");
    check(near(field.sample(264.0F, 8.0F, 0.0F).deep, 1.0F), "the far lake is in the map now");
    check(near(field.center_x(), 264.0F) && near(field.center_z(), 8.0F),
          "the window is centred on the player's chunk");
    // ... and back again: nothing was forgotten.
    field.recenter(0, 0);
    check(near(field.sample(8.0F, 8.0F, 0.0F).deep, 1.0F), "walking back restores the near lake");

    // The window fades out before its edge, so the wrap-around never shows.
    check(near(field.window(field.center_x(), field.center_z()), 1.0F) &&
              near(field.window(field.center_x() + lava_field_fade_start, field.center_z()), 1.0F),
          "the middle of the window is whole");
    check(near(field.window(field.center_x() + lava_field_fade_start + lava_field_fade_width,
                            field.center_z()),
               0.0F) &&
              near(field.window(field.center_x(), field.center_z() - 128.0F), 0.0F),
          "the edge of the window is empty");
    check(lava_field_fade_start + lava_field_fade_width <=
              static_cast<float>(lava_field_extent) * 0.5F - 7.9F,
          "the fade must end inside the window whichever block of the chunk the player is in");
    check(near(field.sample(8.0F + 256.0F, 8.0F, 0.0F).deep, 0.0F),
          "a read one whole map away must not return the near lake");

    // Chunks the renderer no longer keeps are dropped, and their lava with them.
    field.retain(4, 4, 8, 8);
    check(field.section_count() == 0U && field.empty(), "retain must drop what streamed out");
    // Negative coordinates wrap the same way.
    field.recenter(-3, -2);
    field.set_section({-1, -1, 0}, lake(-16, -16, 16, -59));
    check(near(field.sample(-8.0F, -8.0F, 0.0F).deep, 1.0F) &&
              near(field.sample(8.0F, 8.0F, 0.0F).deep, 0.0F),
          "chunks west and north of the origin land in their own slots");
    field.clear();
    check(field.empty() && field.section_count() == 0U, "clear empties the map");
}

void the_map_feeds_the_area_light()
{
    // A lake 48 blocks square under a roof 22 blocks up: the roof over the
    // middle is lit, the roof far from the lake is not, and a point of the
    // roof at the lake's edge sees about half of it.
    LavaField field;
    for (int chunk_z = 0; chunk_z < 3; ++chunk_z)
        for (int chunk_x = 0; chunk_x < 3; ++chunk_x)
            field.set_section({chunk_x, chunk_z, 0}, lake(chunk_x * 16, chunk_z * 16, 16, -59));
    const auto roof_light = [&](float x, float z) {
        constexpr float height = 22.0F;
        const float coverage = field.sample(x, z, lava_footprint_level(height)).deep;
        return lava_far_light(coverage, height, -1.0F);
    };
    const float over_middle = roof_light(24.0F, 24.0F);
    const float over_edge = roof_light(48.0F, 24.0F);
    const float far_away = roof_light(100.0F, 24.0F);
    check(over_middle > 0.25F, "the roof over a lake must be lit by it");
    check(over_edge > over_middle * 0.25F && over_edge < over_middle * 0.75F,
          "the light must fall off across the lake's edge, not stop at it");
    check(near(far_away, 0.0F, 0.002F), "rock far from any lava takes none of its light");
    // Close to the lava the footprint is narrow: a wall two blocks back from
    // the shore and six up reads only what is in front of it.
    const float narrow = field.sample(50.0F, 24.0F, lava_footprint_level(6.0F)).deep;
    const float wide = field.sample(50.0F, 24.0F, lava_footprint_level(30.0F)).deep;
    check(narrow < wide, "height widens what a surface sees");
}

} // namespace

int main()
{
    struct Test final
    {
        const char *name;
        void (*run)();
    };
    constexpr std::array tests{
        Test{"the deep layer arrives with depth, not at a line",
             the_deep_layer_arrives_with_depth_not_at_a_line},
        Test{"exposure opens only in Hell and still closes over lava",
             exposure_opens_only_in_hell_and_still_closes_over_lava},
        Test{"lava lights what faces it and fades with height",
             lava_lights_what_faces_it_and_fades_with_height},
        Test{"the air glows and shimmers above lava only",
             the_air_glows_and_shimmers_above_lava_only},
        Test{"lava breathes slowly", lava_breathes_slowly},
        Test{"the palette is warm but not one red", the_palette_is_warm_but_not_one_red},
        Test{"the first-person body takes the same light",
             the_first_person_body_takes_the_same_light},
        Test{"the lava map holds what the meshes show", the_lava_map_holds_what_the_meshes_show},
        Test{"the lava map follows the player without wrapping",
             the_lava_map_follows_the_player_without_wrapping},
        Test{"the map feeds the area light", the_map_feeds_the_area_light},
    };
    int failures{};
    for (const Test &test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - static_cast<std::size_t>(failures)) << '/' << tests.size()
              << " deep-underground checks passed\n";
    return failures == 0 ? 0 : 1;
}
