// (Wave 5, survival) Included inside Application::Impl (app.cpp). A capture
// aid for mods whose items do something when used (eating, drinking, filling
// a vessel): OPENMINERZ_DIAGNOSTIC_USE_HELD=<seconds>[,<after>] presses the
// use button once, <seconds> of world time into a --world run, exactly as a
// click does (a mod item's `use` runs on the server and its hand animation
// plays), and holds a --screenshot back until <after> seconds later (default
// 0.4), so the still shows what the press started. Nothing without it.

std::optional<std::pair<float, float>> mod_survival_use_; // press at, then capture after
bool mod_survival_use_read_{};
bool mod_survival_used_{};

[[nodiscard]] bool mod_survival_capture_ready()
{
    if (!mod_survival_use_read_)
    {
        mod_survival_use_read_ = true;
        if (const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_USE_HELD"); value != nullptr && *value != '\0')
        {
            const float at = static_cast<float>(std::clamp(std::atof(value), 0.0, 120.0));
            float after = 0.4F;
            if (const char *comma = std::strchr(value, ','); comma != nullptr)
                after = static_cast<float>(std::clamp(std::atof(comma + 1), 0.0, 10.0));
            mod_survival_use_ = std::pair{at, after};
        }
    }
    if (!mod_survival_use_.has_value())
        return true;
    if (!mod_survival_used_)
    {
        if (world_game_time_ < mod_survival_use_->first || !mod_gameplay_live())
            return false;
        mod_survival_used_ = true;
        std::cout << "diagnostic use of the held item at t=" << world_game_time_ << '\n';
        primary_action_pressed();
        mine_button_down_ = false; // a click, not a held button
        return false;
    }
    return world_game_time_ >= mod_survival_use_->first + mod_survival_use_->second;
}
