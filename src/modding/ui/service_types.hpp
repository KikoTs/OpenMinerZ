#pragma once

// The UI service's private records, shared by service.cpp and api.cpp.

#include "openminerz/modding/ui.hpp"

namespace openminerz::modding::ui
{

struct UiService::Screen final
{
    enum class Builtin : std::uint8_t
    {
        none,
        dialogue,
        journal,
        bindings,
        config
    };
    std::uint64_t root{};    // the frame the service draws around the content
    std::uint64_t content{}; // the mod's own tree: the handle scripts hold
    std::string mod;
    Side side{Side::client};
    bool dialog{};
    bool pause{};
    bool dim{true};
    bool closable{true};
    Builtin builtin{Builtin::none};
    Callback on_close;
    Callback on_back;
    // A story dialogue a client mod opened: its choices' handlers.
    std::vector<Callback> choice_callbacks;
    std::vector<std::string> choice_ids;
    Callback on_choice;
    float max_width{1400.0F};
    Surface surface;
};

struct UiService::HudItem final
{
    std::uint64_t root{};
    std::string mod;
    Side side{Side::client};
    bool focus_frame{};
    bool game_frame{}; // (wave 5, survival) frame = "game": no screen margins
};

struct UiService::Toast final
{
    std::vector<TextRun> title;
    std::vector<TextRun> text;
    int item{-1};
    std::optional<Color> color;
    double seconds{4.2};
    double age{};
};

struct UiService::Objective final
{
    std::string id;
    std::string mod;
    std::vector<TextRun> text;
    std::optional<double> progress;
    bool done{};
    bool from_server{};
};

struct UiService::Quest final
{
    std::string id;
    std::string title;
    std::string text;
    std::string state{"active"};
    struct Step final
    {
        std::string text;
        bool done{};
        std::optional<double> progress;
    };
    std::vector<Step> steps;
};

struct UiService::ServerDialogue final
{
    std::string id;
    std::string mod;
    std::vector<std::string> choices;
    Callback callback;
};

} // namespace openminerz::modding::ui
