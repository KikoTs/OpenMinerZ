// Member functions and state of Application::Impl: the Add-Ons screen's
// installed mods - which are on, turning them on and off, and the catalog
// the rest of the game uses (openminerz/modding/addons.hpp). Included inside
// the class body by app.cpp, after the runtime.
//
// When what happens:
//   * scan_addons (startup, entering the Add-Ons screen, the window coming
//     back to the front on it, /mods reload): every root is read mod by mod
//     and the player's choices (addons.cfg beside settings.cfg) applied.
//   * apply_addons: mod_catalog_ becomes the mods that are on, so every other
//     part of the game (VMs, asset stacks, the host and join manifests, the
//     server browser's profile) sees only those, exactly as it used to see
//     every installed mod. The menus' client VM restarts when its mods
//     changed, so themes and packs apply at once.
//   * toggle_addon (a row chosen, or Left / Right on it): refused in a world,
//     while joining and while hosting; otherwise the choice is stored and
//     applied, and a line under the list says what else changed.
//   * the world prompt (mod_world_can_open): a world whose generator mod is
//     off or missing asks before anything loads, and can turn the mod on.
//
// Retail parity: bundled examples start off, so a copy of the game with
// nothing installed by the player loads no mod at all, as before.

modding::AddonScan addon_scan_;
std::vector<modding::AddonStatus> addon_status_;
modding::AddonChoices addon_choices_;
// The rows' labels (the menu's views point into them) and, for each row, the
// index of its mod in addon_scan_.addons (past the end: Open Mods Folder).
std::vector<std::string> addon_row_labels_;
std::vector<std::size_t> addon_row_mods_;
std::vector<std::string> addon_row_ids_; // the same rows by mod id ("*folder")
// What the last change did, under the list until the screen is left.
std::string addon_notice_;
bool addon_notice_warning_{};
float addon_scroll_{};
ui::MenuScreen addon_screen_seen_{ui::MenuScreen::main};

enum class AddonPromptAction : std::uint8_t
{
    none,        // Back only
    turn_on,     // Turn On and Open
    open_folder  // Open Mods Folder
};

// A world that needs a mod which is off or missing (mod_world_can_open).
struct AddonWorldPrompt final
{
    std::filesystem::path world;
    std::string mod_id;
    std::string body;
    AddonPromptAction action{AddonPromptAction::none};
};
std::optional<AddonWorldPrompt> addon_world_prompt_;
std::size_t addon_prompt_focus_{};
std::array<Rect, 2> addon_prompt_buttons_{};
bool addon_prompt_auto_accept_{};
bool addon_focus_settings_{}; // OPENMINERZ_DIAGNOSTIC_ADDONS ">*settings"

[[nodiscard]] static std::filesystem::path addon_choices_path()
{
    return default_settings_path().parent_path() / "addons.cfg";
}

// Reads every mod root again and applies the player's choices.
void scan_addons()
{
    const auto started = std::chrono::steady_clock::now();
    addon_scan_ = modding::scan_addons(modding::addon_roots(modding::bundled_mod_root(application_directory())));
    addon_choices_ = modding::read_addon_choices(addon_choices_path());
    // A script error of a mod that is no longer installed goes with it.
    std::erase_if(mod_script_errors_, [&](const auto &entry) {
        return modding::find_addon(addon_scan_, entry.first) == nullptr;
    });
    for (const std::string &error : addon_scan_.errors)
        std::cerr << "addons.root.error " << error << '\n';
    apply_addons();
    std::cout << "addons.scan installed=" << addon_scan_.addons.size() << " on=" << mod_catalog_.mods.size()
              << " hidden=" << addon_scan_.hidden.size() << " ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
              << '\n';
}

// The mods that are on become the catalog everything else uses.
void apply_addons()
{
    addon_status_ = modding::resolve_addons(addon_scan_, addon_choices_);
    mod_catalog_ = modding::active_catalog(addon_scan_, addon_status_);
    mod_catalog_error_.clear();
    mod_catalog_valid_ = net::make_mod_manifest(mod_catalog_, host_settings_.protocol, host_settings_.mod_manifest,
                                                mod_catalog_error_) &&
                         net::make_mod_manifest(mod_catalog_, net::ProtocolVersion::classic_v3,
                                                join_client_manifest_, mod_catalog_error_);
    if (!mod_catalog_valid_)
    {
        host_settings_.mod_manifest = {};
        host_settings_.mod_manifest.protocol = host_settings_.protocol;
        join_client_manifest_ = {};
        join_client_manifest_.protocol = net::ProtocolVersion::classic_v3;
    }
    join_browser_.set_client(join_client_manifest_, true);
    // Mod runtime area: packages that could not run, and the menus' VM.
    check_mod_packages();
    refresh_addon_rows();
    mod_ui_menu_stale_ = true; // the settings rows follow (app_mod_ui.inc)
}

// Whether a mod's settings page is offered: it is on, or runs in the session.
[[nodiscard]] bool mod_settings_shown(std::string_view id) const
{
    // Mod ids compare without case, as everywhere in the catalog.
    const auto same = [id](std::string_view other) {
        return std::equal(id.begin(), id.end(), other.begin(), other.end(), [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
        });
    };
    if (std::any_of(mod_catalog_.mods.begin(), mod_catalog_.mods.end(),
                    [&](const net::LoadedMod &mod) { return same(mod.descriptor.id); }))
        return true;
    if (mod_session_ == ModSessionKind::none || !mod_runtime_)
        return false;
    const std::vector<modding::ModStatus> running = mod_runtime_->status();
    return std::any_of(running.begin(), running.end(), [&](const modding::ModStatus &status) { return same(status.id); });
}

// What a join may use: the mods that are on, plus server mods that are off,
// for a host that requires them (net::select_session_mods).
[[nodiscard]] net::ModCatalog addon_join_catalog() const
{
    return mod_catalog_valid_ ? modding::join_catalog(addon_scan_, addon_status_) : net::ModCatalog{};
}

// The rows of the screen: one per installed mod, then Open Mods Folder. The
// focus stays on the mod it was on when the list changes around it.
void refresh_addon_rows()
{
    // By id: a scan may have put the mods in other places.
    std::string focused;
    if (const std::optional<std::size_t> row = menu_.selected_addon_entry(); row && *row < addon_row_ids_.size())
        focused = addon_row_ids_[*row];
    std::vector<std::string> labels;
    std::vector<std::size_t> mods;
    std::vector<std::string> ids;
    for (std::size_t index{}; index < addon_scan_.addons.size(); ++index)
    {
        labels.push_back(addon_scan_.addons[index].info.name);
        mods.push_back(index);
        ids.push_back(addon_scan_.addons[index].id());
    }
    labels.emplace_back("Open Mods Folder");
    mods.push_back(addon_scan_.addons.size());
    ids.emplace_back("*folder");
    std::vector<ui::MenuEntry> entries;
    for (std::size_t row{}; row < labels.size(); ++row)
        entries.push_back({labels[row],
                           mods[row] < addon_scan_.addons.size() ? "Turn this add-on on or off."
                                                                  : "Open the folder add-ons are installed in.",
                           mods[row] < addon_scan_.addons.size() ? ui::MenuCommand::toggle_addon
                                                                  : ui::MenuCommand::open_mods_folder,
                           true,
                           {}});
    menu_.set_addon_entries(std::move(entries));
    // The menu now points into `labels`; moving the vector keeps its strings.
    addon_row_labels_ = std::move(labels);
    addon_row_mods_ = std::move(mods);
    addon_row_ids_ = std::move(ids);
    if (focused.empty() || menu_.screen() != ui::MenuScreen::addons)
        return;
    const auto &all = menu_.entries();
    const auto first = std::find_if(all.begin(), all.end(), [](const ui::MenuEntry &entry) {
        return entry.command == ui::MenuCommand::toggle_addon || entry.command == ui::MenuCommand::open_mods_folder;
    });
    for (std::size_t row{}; row < addon_row_ids_.size() && first != all.end(); ++row)
        if (addon_row_ids_[row] == focused)
        {
            menu_.set_selection(static_cast<std::size_t>(first - all.begin()) + row, settings_);
            break;
        }
}

// Why add-ons cannot be turned on or off right now ("" = they can). A
// session runs the mods it started with; a join or a hosted game has told
// other players which mods it runs.
[[nodiscard]] std::string addons_locked_reason() const
{
    if (mod_session_ != ModSessionKind::none || menu_.pause_overlay_active() || online_client_joined_)
        return "Leave the world to turn add-ons on or off. Changes apply to the next world you start, load, "
               "host or join.";
    if (online_join_in_progress())
        return "A join is in progress. Cancel it to turn add-ons on or off.";
    if (online_host_ != nullptr || online_host_start_.valid())
        return "Stop hosting to turn add-ons on or off.";
    return {};
}

// Entering the screen and coming back to the game while it is open: mods
// copied in or removed meanwhile show up.
void refresh_addons_from_disk()
{
    if (addons_locked_reason().empty())
        scan_addons();
}

// Every frame: the notice and the scroll belong to one visit of the screen.
void update_addon_screen()
{
    if (addon_prompt_auto_accept_ && addon_world_prompt_.has_value())
    {
        addon_prompt_auto_accept_ = false;
        answer_addon_prompt(true);
    }
    if (addon_focus_settings_ && menu_.screen() == ui::MenuScreen::addons && !menu_.entries().empty() &&
        menu_.entries().back().command == ui::MenuCommand::mod_entry)
    {
        addon_focus_settings_ = false;
        menu_.set_selection(menu_.entries().size() - 1U, settings_);
    }
    const ui::MenuScreen screen = menu_.screen();
    if (screen == addon_screen_seen_)
        return;
    if (screen == ui::MenuScreen::addons)
    {
        addon_scroll_ = 0.0F;
        refresh_addons_from_disk();
    }
    else
        addon_notice_.clear();
    addon_screen_seen_ = screen;
}

void addon_notice(std::string text, bool warning)
{
    addon_notice_ = std::move(text);
    addon_notice_warning_ = warning;
}

// The mod a row names, if it is a mod's row.
[[nodiscard]] const modding::InstalledAddon *selected_addon(std::size_t *index = nullptr) const
{
    const std::optional<std::size_t> row = menu_.selected_addon_entry();
    if (!row || *row >= addon_row_mods_.size() || addon_row_mods_[*row] >= addon_scan_.addons.size())
        return nullptr;
    if (index != nullptr)
        *index = addon_row_mods_[*row];
    return &addon_scan_.addons[addon_row_mods_[*row]];
}

// When a change takes effect, in the words the screen uses.
[[nodiscard]] static std::string_view addon_applies(const modding::InstalledAddon &addon)
{
    return addon.mod.descriptor.scope == net::ModScope::client
               ? "It applies now."
               : "It applies to the next world you start, load or host.";
}

// Turns the mod at `index` on or off: stored, applied, and said. Returns
// whether it is now as asked.
bool set_addon_state(std::size_t index, bool on, bool announce = true)
{
    if (index >= addon_scan_.addons.size())
        return false;
    // A copy, so the words below never read a scan that changed under them.
    const modding::InstalledAddon addon = addon_scan_.addons[index];
    // (The screen's first line says why.)
    if (!addons_locked_reason().empty())
    {
        addon_notice(std::format("{} was not changed.", addon.info.name), true);
        return false;
    }
    if (addon_status_[index].on == on)
        return true;
    if (!addon.usable())
    {
        addon_notice(std::format("{} cannot be turned on: {}", addon.info.name, addon.problem), true);
        return false;
    }
    modding::AddonChange change = modding::set_addon(addon_scan_, addon_choices_, addon.id(), on);
    std::string error;
    const bool saved = modding::write_addon_choices(addon_choices_path(), change.choices, error);
    addon_choices_ = std::move(change.choices);
    mod_script_errors_.erase(addon.id());
    apply_addons();
    const bool now = index < addon_status_.size() && addon_status_[index].on;
    std::string text = now == on ? std::format("{} is {}.", addon.info.name, on ? "on" : "off")
                                 : std::format("{} stays off: {}", addon.info.name, addon_status_[index].reason);
    for (const std::string &notice : change.notices)
        text += ' ' + notice;
    if (now == on)
        text += std::format(" {}", addon_applies(addon));
    if (!saved)
        text += " (Not saved: " + error + ")";
    std::cout << "addons.toggle id=" << addon.id() << " on=" << now << " changed=" << change.notices.size()
              << (saved ? "" : " unsaved") << '\n';
    for (const std::string &notice : change.notices)
        std::cout << "addons.notice " << notice << '\n';
    if (announce)
        addon_notice(std::move(text), !change.notices.empty() || now != on || !saved);
    if (!options_.screenshot_path.has_value() && sound_bank_ != nullptr)
        mod_ui_menu_cue("Click");
    return now == on;
}

// MenuCommand::toggle_addon from handle().
void toggle_selected_addon()
{
    std::size_t index{};
    if (selected_addon(&index) != nullptr)
        static_cast<void>(set_addon_state(index, !addon_status_[index].on));
}

// Left and Right on a mod's row: off and on.
bool adjust_selected_addon(int direction)
{
    std::size_t index{};
    if (menu_.screen() != ui::MenuScreen::addons || selected_addon(&index) == nullptr)
        return false;
    static_cast<void>(set_addon_state(index, direction > 0));
    return true;
}

// %LOCALAPPDATA%\OpenMinerZ\Mods in Explorer, made if it is missing.
void open_mods_folder()
{
    const std::filesystem::path folder = modding::user_mod_root();
    if (folder.empty())
    {
        addon_notice("The Mods folder could not be found: the local app data folder is unknown.", true);
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        addon_notice(std::format("The Mods folder could not be made: {}", error.message()), true);
        return;
    }
    const std::u8string path = folder.u8string();
    const std::string text{path.begin(), path.end()};
    std::cout << "addons.open_folder " << text << '\n';
    addon_notice("Opened the Mods folder. Copy an add-on's folder into it; this list updates when you come "
                 "back to the game.",
                 false);
    // Captures and benchmarks never open windows of other programs.
    if (options_.screenshot_path.has_value() || options_.bench_frames > 0)
        return;
    if (!SDL_OpenURL(text.c_str()))
        addon_notice(std::format("The Mods folder is {} (it could not be opened: {}).", text, SDL_GetError()), true);
}

// ---- What the screen says about a mod -------------------------------------------

// "A", "A and B", "A, B and C".
[[nodiscard]] static std::string addon_names_listed(const std::vector<std::string> &names)
{
    std::string text;
    for (std::size_t name{}; name < names.size(); ++name)
        text += std::string{name == 0U ? "" : name + 1U == names.size() ? " and " : ", "} + names[name];
    return text;
}

enum class AddonRowState : std::uint8_t
{
    on,
    off,
    not_loaded,
    script_error
};

[[nodiscard]] AddonRowState addon_row_state(std::size_t index) const
{
    const modding::InstalledAddon &addon = addon_scan_.addons[index];
    if (!addon.usable())
        return AddonRowState::not_loaded;
    if (index >= addon_status_.size() || !addon_status_[index].on)
        return AddonRowState::off;
    if (mod_refusals_.contains(addon.id()))
        return AddonRowState::not_loaded;
    if (mod_script_errors_.contains(addon.id()))
        return AddonRowState::script_error;
    return AddonRowState::on;
}

struct AddonDetail final
{
    std::string facts;       // version, kind, where it is installed
    std::string author;
    std::string state;       // on or off, and when that applies, or what is wrong
    bool state_warning{};
    std::string multiplayer; // what it means for the games you host and join
    std::string notes;       // world generator, conflicts, dependencies, settings
    std::string folder;
};

[[nodiscard]] AddonDetail addon_detail(std::size_t index) const
{
    const modding::InstalledAddon &addon = addon_scan_.addons[index];
    const bool client = addon.mod.descriptor.scope == net::ModScope::client;
    AddonDetail detail;
    const std::string kind = client ? "Client add-on"
                             : addon.mod.descriptor.scope == net::ModScope::core ? "Core add-on"
                                                                                 : "Server add-on";
    // The game versions only when it is not both.
    const std::string games = addon.mod.classic && addon.mod.modern ? std::string{}
                              : addon.mod.classic                    ? std::string{"   -   Classic only"}
                                                                     : std::string{"   -   Modern only"};
    detail.facts = addon.usable() ? std::format("Version {}   -   {}{}   -   {}", addon.mod.descriptor.version, kind,
                                                games, modding::addon_origin_label(addon.origin))
                                  : std::string{modding::addon_origin_label(addon.origin)};
    if (!addon.info.author.empty())
        detail.author = "By " + addon.info.author;
    const std::u8string folder = addon.mod.root.u8string();
    detail.folder = "Folder: " + std::string{folder.begin(), folder.end()};
    const auto refusal = mod_refusals_.find(addon.id());
    const auto script_error = mod_script_errors_.find(addon.id());
    const modding::AddonStatus status = index < addon_status_.size() ? addon_status_[index] : modding::AddonStatus{};
    switch (addon_row_state(index))
    {
    case AddonRowState::not_loaded:
        detail.state = std::format("Not loaded: {}", refusal != mod_refusals_.end() ? refusal->second : addon.problem);
        detail.state_warning = true;
        break;
    case AddonRowState::script_error:
        detail.state = std::format("On, but a script failed: {}", script_error->second);
        detail.state_warning = true;
        break;
    case AddonRowState::off: {
        if (status.wanted && !status.reason.empty())
        {
            detail.state = "Off: " + status.reason;
            detail.state_warning = true;
            break;
        }
        std::string key = addon.id();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        detail.state = addon.origin == modding::AddonOrigin::bundled && !addon_choices_.contains(key)
                           ? "Off. Bundled examples start off: choose it to turn it on."
                           : "Off. Choose it to turn it on.";
        // What is on now and would go off with it.
        std::vector<std::string> blocking;
        for (std::size_t other{}; other < addon_scan_.addons.size() && other < addon_status_.size(); ++other)
            if (addon_status_[other].on &&
                modding::addon_clash(addon, addon_scan_.addons[other]) != modding::AddonClash::none)
                blocking.push_back(addon_scan_.addons[other].info.name);
        if (!blocking.empty())
            detail.state += std::format(" That turns off {}.", addon_names_listed(blocking));
        break;
    }
    case AddonRowState::on:
        detail.state = client                              ? "On. It applies now, in the menus and in every world."
                       : mod_session_ == ModSessionKind::none ? "On. It runs in the next world you start, load or host."
                                                              : "On. It runs in this world.";
        break;
    }
    // (A descriptor that could not be read says nothing about its kind; a
    // read one always has a version.)
    if (!addon.mod.descriptor.version.empty())
        detail.multiplayer =
            client ? "Client add-on: it changes only what you see and hear, so it works on every server, the "
                     "original game's too. Other players do not need it."
                   : "Server add-on: it runs in your own worlds and in games you host. While it is on, games you "
                     "host admit only OpenMinerZ players, who get it from you when they join. Joining an "
                     "OpenMinerZ host that runs it uses your copy, even while it is off here.";
    std::vector<std::string> notes;
    if (addon.defines_world)
        notes.emplace_back("It replaces the world generator: new worlds get its terrain, and worlds made with it "
                           "need it on.");
    std::vector<std::string> clashes;
    for (const modding::InstalledAddon &other : addon_scan_.addons)
        if (modding::addon_clash(addon, other) != modding::AddonClash::none)
            clashes.push_back(other.info.name);
    const auto listed = [](const std::vector<std::string> &names) { return addon_names_listed(names); };
    if (!clashes.empty())
        notes.push_back(std::format("It cannot be on together with {}.", listed(clashes)));
    std::vector<std::string> needs;
    std::vector<std::string> missing;
    for (const modding::PackageDependency &dependency : addon.info.depends)
    {
        const modding::InstalledAddon *needed = modding::find_addon(addon_scan_, dependency.id);
        if (needed != nullptr)
            needs.push_back(needed->info.name);
        else
            missing.push_back(dependency.id);
    }
    if (!needs.empty())
        notes.push_back(std::format("It needs {}, which come{} on with it.", listed(needs), needs.size() == 1U ? "s" : ""));
    if (!missing.empty())
        notes.push_back(std::format("It needs {}, which {} not installed.", listed(missing),
                                    missing.size() == 1U ? "is" : "are"));
    if (std::any_of(addon_scan_.hidden.begin(), addon_scan_.hidden.end(),
                    [&](const std::string &id) { return id == addon.id(); }))
        notes.emplace_back("Your copy replaces the bundled example of the same add-on.");
    if (mod_runtime_ != nullptr && mod_settings_shown(addon.id()))
    {
        const std::vector<modding::ConfigSetting> settings = mod_runtime_->config_settings();
        if (std::any_of(settings.begin(), settings.end(),
                        [&](const modding::ConfigSetting &setting) { return setting.mod_id == addon.id(); }))
            notes.emplace_back("Its settings are under Add-On Settings.");
    }
    for (const std::string &note : notes)
        detail.notes += (detail.notes.empty() ? "" : " ") + note;
    return detail;
}

// OPENMINERZ_DIAGNOSTIC_ADDONS=<step>[,<step>...], for captures of the
// Add-Ons screen: "+id" turns a mod on and "-id" off as choosing its row
// would (stored in the run's addons.cfg), ">id" puts the focus on its row,
// ">*folder" on Open Mods Folder, ">*settings" on the last row a mod added.
void run_addon_diagnostic()
{
    const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_ADDONS");
    if (value == nullptr)
        return;
    std::string_view steps{value};
    while (!steps.empty())
    {
        const std::size_t comma = steps.find(',');
        const std::string_view step = steps.substr(0U, comma);
        steps = comma == std::string_view::npos ? std::string_view{} : steps.substr(comma + 1U);
        if (step.size() < 2U)
            continue;
        if (step == ">*folder")
        {
            const auto &entries = menu_.entries();
            for (std::size_t entry{}; entry < entries.size(); ++entry)
                if (entries[entry].command == ui::MenuCommand::open_mods_folder)
                    menu_.set_selection(entry, settings_);
            continue;
        }
        // The rows mods add arrive with the menus' VM, a frame later.
        if (step == ">*settings")
        {
            addon_focus_settings_ = true;
            continue;
        }
        std::size_t index{};
        while (index < addon_scan_.addons.size() && addon_scan_.addons[index].id() != step.substr(1U))
            ++index;
        if (index >= addon_scan_.addons.size())
            continue;
        if (step.front() == '+' || step.front() == '-')
            static_cast<void>(set_addon_state(index, step.front() == '+'));
        else if (step.front() == '>')
        {
            const auto &entries = menu_.entries();
            const auto first = std::find_if(entries.begin(), entries.end(), [](const ui::MenuEntry &entry) {
                return entry.command == ui::MenuCommand::toggle_addon;
            });
            if (first != entries.end())
                menu_.set_selection(static_cast<std::size_t>(first - entries.begin()) + index, settings_);
        }
    }
}

// ---- The world prompt ---------------------------------------------------------

// load_world refused `world`: its terrain comes from `record`'s mod, which
// is off or missing. Asks, over the world list.
void addon_prompt_world(const std::filesystem::path &world, const modding::worldgen::GeneratorRecord &record)
{
    AddonWorldPrompt prompt;
    prompt.world = world;
    prompt.mod_id = record.mod_id;
    const modding::InstalledAddon *addon = modding::find_addon(addon_scan_, record.mod_id);
    const std::string version = record.mod_version.empty() ? std::string{} : " " + record.mod_version;
    if (addon == nullptr)
    {
        prompt.body = std::format("This world was made with the add-on \"{}\"{}, which is not installed. Put its "
                                  "folder in your Mods folder to open the world.",
                                  record.mod_id, version);
        prompt.action = AddonPromptAction::open_folder;
    }
    else if (!addon->usable())
        prompt.body = std::format("This world was made with {}, which cannot load: {}", addon->info.name,
                                  addon->problem);
    else if (const std::string locked = addons_locked_reason(); !locked.empty())
        prompt.body = std::format("This world was made with {}, which is off. {}", addon->info.name, locked);
    else
    {
        prompt.body = std::format("This world was made with {}, which is off. Turn it on to open the world: "
                                  "without it, new places in the world would be made with retail terrain.",
                                  addon->info.name);
        // What else the switch would change, before it is made.
        const modding::AddonChange preview = modding::set_addon(addon_scan_, addon_choices_, addon->id(), true);
        const std::vector<modding::AddonStatus> after = modding::resolve_addons(addon_scan_, preview.choices);
        std::vector<std::string> going;
        std::vector<std::string> coming;
        for (std::size_t index{}; index < after.size() && index < addon_status_.size(); ++index)
        {
            if (addon_status_[index].on && !after[index].on)
                going.push_back(addon_scan_.addons[index].info.name);
            else if (!addon_status_[index].on && after[index].on && addon_scan_.addons[index].id() != addon->id())
                coming.push_back(addon_scan_.addons[index].info.name);
        }
        if (!going.empty())
            prompt.body += std::format(" Turning it on turns off {}.", addon_names_listed(going));
        if (!coming.empty())
            prompt.body += std::format(" It also turns on {}, which it needs.", addon_names_listed(coming));
        prompt.action = AddonPromptAction::turn_on;
    }
    std::cout << "addons.world_prompt mod=" << record.mod_id << " installed=" << (addon != nullptr) << '\n';
    addon_world_prompt_ = std::move(prompt);
    addon_prompt_focus_ = 0U;
    // OPENMINERZ_DIAGNOSTIC_ADDON_PROMPT=accept answers it on the next frame,
    // as Enter would (never from inside the load that refused the world).
    const char *answer = SDL_getenv("OPENMINERZ_DIAGNOSTIC_ADDON_PROMPT");
    addon_prompt_auto_accept_ = answer != nullptr && std::string_view{answer} == "accept";
    // Over the world list, with the world chosen, rather than over nothing.
    if (menu_.screen() == ui::MenuScreen::world_preview || menu_.screen() == ui::MenuScreen::main)
    {
        refresh_world_list();
        menu_.open(ui::MenuScreen::world_select);
        for (std::size_t slot{}; slot < world_slots_.size(); ++slot)
            if (world_slots_[slot].path == world)
                menu_.set_selection(slot + 1U, settings_);
    }
}

void answer_addon_prompt(bool accept)
{
    if (!addon_world_prompt_.has_value())
        return;
    const AddonWorldPrompt prompt = std::move(*addon_world_prompt_);
    addon_world_prompt_.reset();
    if (!accept || prompt.action == AddonPromptAction::none)
        return;
    if (prompt.action == AddonPromptAction::open_folder)
    {
        open_mods_folder();
        return;
    }
    std::size_t index{};
    for (; index < addon_scan_.addons.size(); ++index)
        if (addon_scan_.addons[index].id() == prompt.mod_id)
            break;
    if (!set_addon_state(index, true, false))
    {
        toast("ADD-ON NOT TURNED ON", addon_notice_.empty() ? std::string{"It could not be turned on."} : addon_notice_);
        return;
    }
    std::cout << "addons.world_prompt accepted mod=" << prompt.mod_id << '\n';
    menu_.open(ui::MenuScreen::world_preview);
    load_world(prompt.world);
}

// The prompt has the keys and the pad while it is up; the pointer reaches
// its buttons through the hit list (click_at).
bool addon_prompt_event(const SDL_Event &event)
{
    if (!addon_world_prompt_.has_value())
        return false;
    const std::size_t buttons = addon_world_prompt_->action == AddonPromptAction::none ? 1U : 2U;
    const auto choose = [&](std::size_t focus) {
        answer_addon_prompt(buttons == 2U && focus == 0U);
    };
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_MOTION:
        for (std::size_t button{}; button < buttons; ++button)
            if (addon_prompt_buttons_[button].contains(event.motion.x, event.motion.y))
                addon_prompt_focus_ = button;
        return false;
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.key)
        {
        case SDLK_LEFT:
        case SDLK_RIGHT:
        case SDLK_A:
        case SDLK_D:
        case SDLK_TAB:
            addon_prompt_focus_ = (addon_prompt_focus_ + 1U) % buttons;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!event.key.repeat)
                choose(addon_prompt_focus_);
            break;
        case SDLK_ESCAPE:
            answer_addon_prompt(false);
            break;
        default:
            break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
            addon_prompt_focus_ = (addon_prompt_focus_ + 1U) % buttons;
        else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
            choose(addon_prompt_focus_);
        else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
            answer_addon_prompt(false);
        return true;
    default:
        return false;
    }
}
