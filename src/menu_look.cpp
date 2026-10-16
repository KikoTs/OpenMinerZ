#include "openminerz/menu_look.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <utility>

namespace openminerz::ui
{

std::string menu_title_case(std::string_view value)
{
    using Fixed = std::pair<std::string_view, std::string_view>;
    static constexpr std::array<Fixed, 16> fixed{
        {{"FPS", "FPS"}, {"HUD", "HUD"}, {"HD", "HD"}, {"PVP", "PVP"}, {"VSYNC", "VSync"},
         {"TAA", "TAA"}, {"FXAA", "FXAA"}, {"MSAA", "MSAA"}, {"ACES", "ACES"}, {"UI", "UI"},
         {"HDR", "HDR"}, {"SSAO", "SSAO"}, {"MS", "ms"}, {"X", "x"}, {"OK", "OK"}, {"ID", "ID"}}};
    static constexpr std::array<std::string_view, 14> small{
        "a", "an", "and", "at", "by", "for", "in", "of", "on", "or", "per", "the", "to", "with"};
    const auto word_character = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '\'';
    };
    std::string result;
    result.reserve(value.size());
    bool first = true;
    for (std::size_t index = 0U; index < value.size();)
    {
        // Anything that is not part of a word - spaces, punctuation, the bytes
        // of a character outside ASCII - passes through untouched.
        if (!word_character(value[index]))
        {
            result += value[index++];
            continue;
        }
        std::size_t end = index;
        while (end < value.size() && word_character(value[end]))
            ++end;
        std::string lower{value.substr(index, end - index)};
        std::string upper = lower;
        bool digit = false;
        for (std::size_t letter = 0U; letter < lower.size(); ++letter)
        {
            const auto character = static_cast<unsigned char>(lower[letter]);
            digit = digit || std::isdigit(character) != 0;
            lower[letter] = static_cast<char>(std::tolower(character));
            upper[letter] = static_cast<char>(std::toupper(character));
        }
        const auto known = std::ranges::find(fixed, std::string_view{upper}, &Fixed::first);
        if (known != fixed.end())
            result += known->second;
        else if (digit ||
                 (!first && std::ranges::find(small, std::string_view{lower}) != small.end()))
            result += lower;
        else
        {
            lower[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(lower[0])));
            result += lower;
        }
        first = false;
        index = end;
    }
    return result;
}

std::string menu_value_case(std::string value)
{
    const bool mixed = std::ranges::any_of(value, [](char character) {
        return std::islower(static_cast<unsigned char>(character)) != 0;
    });
    return mixed ? value : menu_title_case(value);
}

std::string menu_plain_punctuation(std::string_view value)
{
    using Plain = std::pair<std::string_view, std::string_view>;
    // UTF-8 of each character, and what stands in for it.
    static constexpr std::array<Plain, 11> plain{
        {{"\xE2\x80\xA2", "-"},   // bullet
         {"\xE2\x80\x93", "-"},   // en dash
         {"\xE2\x80\x94", "-"},   // em dash
         {"\xE2\x80\x98", "'"},   // left single quote
         {"\xE2\x80\x99", "'"},   // right single quote
         {"\xE2\x80\x9C", "\""},  // left double quote
         {"\xE2\x80\x9D", "\""},  // right double quote
         {"\xE2\x80\xA6", "..."}, // ellipsis
         {"\xE2\x80\xB9", "<"},   // single left angle quote
         {"\xE2\x80\xBA", ">"},   // single right angle quote
         {"\xC2\xA0", " "}}};     // no-break space
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0U; index < value.size();)
    {
        const auto found = std::ranges::find_if(plain, [&](const Plain &entry) {
            return value.substr(index, entry.first.size()) == entry.first;
        });
        if (found != plain.end())
        {
            result += found->second;
            index += found->first.size();
        }
        else
            result += value[index++];
    }
    return result;
}

MenuLetterChoice choose_menu_letter(std::span<const int> family_caps, float wanted_cap) noexcept
{
    MenuLetterChoice choice{family_caps.size(), 1.0F};
    wanted_cap = std::max(wanted_cap, 1.0F);
    for (std::size_t index = 0U; index < family_caps.size(); ++index)
    {
        if (family_caps[index] <= 0)
            continue;
        choice.member = index;
        // Large enough (or as good as): stop at the first such member.
        if (static_cast<float>(family_caps[index]) * 1.06F >= wanted_cap)
            break;
    }
    if (choice.member >= family_caps.size())
        return choice;
    choice.scale = wanted_cap / static_cast<float>(family_caps[choice.member]);
    if (std::abs(choice.scale - 1.0F) < 0.06F)
        choice.scale = 1.0F;
    return choice;
}

float menu_focus_amount(float seconds_since_focus) noexcept
{
    const float phase = std::fmod(std::max(0.0F, seconds_since_focus), 0.5F) / 0.25F;
    return phase <= 1.0F ? 1.0F - phase : phase - 1.0F;
}

PictureCrop cover_crop(float picture_width, float picture_height, float window_width,
                       float window_height) noexcept
{
    if (!(picture_width > 0.0F) || !(picture_height > 0.0F) || !(window_width > 0.0F) ||
        !(window_height > 0.0F))
        return {0.0F, 0.0F, std::max(picture_width, 0.0F), std::max(picture_height, 0.0F)};
    const float scale = std::max(window_width / picture_width, window_height / picture_height);
    const float shown_width = std::min(picture_width, window_width / scale);
    const float shown_height = std::min(picture_height, window_height / scale);
    return {(picture_width - shown_width) * 0.5F, (picture_height - shown_height) * 0.5F,
            shown_width, shown_height};
}

} // namespace openminerz::ui
