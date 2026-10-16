// Included inside Application::Impl (src/app.cpp).
//
// THE MENUS' LOOK
//
// OpenMinerZ's own screens - title, world library, server browser, settings,
// the in-game menu, dialogs - are set in CastleMiner Z's lettering and follow
// its habits. What those are was recorded from the shipped builds rather than
// remembered: tools/OriginalClientInterop/MenusParityProbe runs the original
// MainMenu, GameModeMenu, DifficultyLevelScreen, OptionsMenu / OptionsScreen,
// SettingsMenu and InGameMenu offline and writes every string and sprite they
// submit, with a frame of each. In short:
//
//   1.6.3.8  Menu rows are LargeFont (a rounded display face, capitals 36 px
//            on the 1280x720 canvas), white with a two-pixel black outline.
//            The focused row runs to Color.Red and back twice a second. Rows
//            are right-aligned in the left half from y = 175, 54 px apart;
//            the focused row's description stands at (715, 200) in
//            MedLargeFont (a bold humanist face, capitals 23 px). A heading is
//            one LargeFont line centred at the top. Settings rows are
//            MedLargeFont: label on the left, "<  value  >" on the right, the
//            focused row solid red, bars black with a white fill. Lists (worlds,
//            servers) are MedFont with SmallFont details. The in-game menu is
//            the same row list centred over a 50% black wash.
//   1.9.9.8  The same structure in a condensed face: rows CMZColors.MenuGreen
//            (78, 177, 61) running to white, headings green, tabs and buttons
//            green at 80%, everything multiplied by window height / 720.
//   Both     One cue, "Click", for moving and for choosing, "Popup" when a
//            dialog opens. No panels behind menu text: the outline carries it
//            over the backdrop, which in both builds is a picture of the tower
//            that stands at the centre of every world.
//
// OpenMinerZ keeps its own screens and options, so nothing here replicates a
// retail screen; this is that vocabulary applied to ours. All lengths below
// are in "units": pixels of that 1280x720 canvas (menu_unit()), so a screen
// reads the same at 720p, 1080p and 2160p, and on 4:3 the width decides.
// Lettering sizes are the capital heights the shipped fonts exist at (11, 18,
// 23, 36), which is why 720p text is the original bitmaps untouched.

// ------------------------------------------------------------- lettering

enum class MenuFace : std::uint8_t
{
    display, // rows, headings, buttons, names: LargeFont's family
    text     // descriptions, values, notes: MedLargeFont's family
};

static constexpr float menu_cap_small = 11.0F;
static constexpr float menu_cap_medium = 18.0F;
static constexpr float menu_cap_text = 23.0F;
static constexpr float menu_cap_large = 36.0F;

// Window pixels per unit of the 1280x720 canvas. metrics_.scale is relative
// to 1920x1080 and already carries the interface-scale setting and the
// shrink that keeps a screen inside its margins.
[[nodiscard]] float menu_unit() const noexcept
{
    return metrics_.scale * 1.5F;
}

// A capital height in window pixels, for a height given in units.
[[nodiscard]] float menu_cap(float units) const noexcept
{
    return units * menu_unit() * (settings_.larger_text ? 1.12F : 1.0F);
}

// Which shipped build's lettering and colours the menus use. An explicit
// Classic or Modern interface style is that build's. Otherwise a world shows
// its own generation's (so the in-game menu matches the game being played) and
// the front end the installed content pack's.
[[nodiscard]] RetailGeneration menu_generation() const noexcept
{
    if (settings_.ui_style == UiStyle::classic)
        return RetailGeneration::classic;
    if (settings_.ui_style == UiStyle::modern)
        return RetailGeneration::modern;
    if (menu_.screen() == ui::MenuScreen::world_preview || menu_.pause_overlay_active())
        return inventory_.profile() == InventoryProfile::modern ? RetailGeneration::modern
                                                                : RetailGeneration::classic;
    return loaded_asset_style_ == AssetStyle::modern ? RetailGeneration::modern
                                                     : RetailGeneration::classic;
}

// That build's sprite fonts, or null when its content pack was imported
// before fonts were: the menus then use the built-in UI font at the same
// sizes, and say so once.
[[nodiscard]] RetailFonts *menu_fonts() noexcept
{
    RetailFonts *fonts = menu_generation() == RetailGeneration::modern ? modern_fonts_.get()
                                                                       : classic_fonts_.get();
    // OPENMINERZ_DIAGNOSTIC_MENU_UI_FONT=1 shows that fallback on a machine
    // whose packs do have the fonts.
    if (fonts != nullptr && fonts->complete() &&
        SDL_getenv("OPENMINERZ_DIAGNOSTIC_MENU_UI_FONT") == nullptr)
        return fonts;
    if (!menu_font_notice_)
    {
        menu_font_notice_ = true;
        std::cerr << "Menus: the "
                  << (menu_generation() == RetailGeneration::modern ? "Modern" : "Classic")
                  << " content pack has no imported sprite fonts, so the menus are set in the "
                     "built-in UI font. Re-import the game content to restore CastleMiner Z's "
                     "lettering.\n";
    }
    return nullptr;
}

struct MenuInk final
{
    SDL_Color row;      // a menu row at rest
    SDL_Color focus;    // what the focused row runs to
    SDL_Color heading;  // screen headings
    SDL_Color text;     // descriptions, values
    SDL_Color quiet;    // secondary lines, hints
    SDL_Color disabled; // Color.Gray, as the originals grey a locked mode
    SDL_Color accent;   // bars, marks, the things that are not lettering
    SDL_Color warning;  // a line that needs attention
};

[[nodiscard]] MenuInk menu_ink() const noexcept
{
    const SDL_Color white_ink{255U, 255U, 255U, 255U};
    const SDL_Color quiet = settings_.high_contrast ? SDL_Color{240U, 240U, 240U, 255U}
                                                    : SDL_Color{205U, 208U, 210U, 255U};
    // (UI area: mod_ui_menu_ink returns these unchanged unless a mod theme
    // recolours them; app_mod_ui.inc.)
    if (menu_generation() == RetailGeneration::modern)
    {
        // CMZColors.MenuGreen running to white.
        const SDL_Color green{78U, 177U, 61U, 255U};
        return mod_ui_menu_ink({green, white_ink, green, white_ink, quiet, {128U, 128U, 128U, 255U}, green,
                                {255U, 189U, 0U, 255U}});
    }
    // White running to Color.Red.
    return mod_ui_menu_ink({white_ink, {255U, 0U, 0U, 255U}, white_ink, white_ink, quiet,
                            {128U, 128U, 128U, 255U}, {255U, 0U, 0U, 255U}, {255U, 96U, 96U, 255U}});
}

// MenuScreen.OnDraw's focus colour: Color.Lerp(row, selected, amount) where
// `amount` falls from one to zero in a quarter second and climbs back. The
// run restarts when the focus moves, so a newly focused row is at full
// strength. Reduced motion holds it there.
[[nodiscard]] SDL_Color menu_focus_color() const noexcept
{
    const MenuInk ink = menu_ink();
    if (settings_.reduced_motion)
        return ink.focus;
    const float amount = ui::menu_focus_amount(elapsed_ - menu_focus_since_);
    const auto mix = [amount](Uint8 from, Uint8 to) {
        return static_cast<Uint8>(std::lround(static_cast<float>(from) +
                                              (static_cast<float>(to) - static_cast<float>(from)) *
                                                  amount));
    };
    return {mix(ink.row.r, ink.focus.r), mix(ink.row.g, ink.focus.g), mix(ink.row.b, ink.focus.b),
            255U};
}

// One piece of lettering, resolved: which font draws it and how large.
struct MenuLetter final
{
    RetailFonts *fonts{}; // null: FontRenderer at `pixels`
    RetailFont font{};
    float scale{1.0F};
    int pixels{12};
    float cap{};
    FontRenderer *ttf{}; // UI area: a mod theme's face in place of font_
};

// The font that sets `value` with capitals `cap` window pixels tall.
//
// Each build's fonts come in families of one typeface at several sizes
// (1.6.3.8: Small/Med/Large are the rounded face, MyriadSmall/Medium/Large and
// MedLarge the bold one; 1.9.9.8: Small/Med/MedLarge/Large are all its
// condensed face). The smallest member at least as large as the request is
// used and reduced, which keeps the glyphs sharp; only a request above the
// largest member is enlarged. Text with a character the font lacks (a player
// or world name in another script) is set in the UI font rather than as
// asterisks.
[[nodiscard]] MenuLetter menu_letter(MenuFace face, float cap, std::string_view value)
{
    if (menu_cap_ratio_ <= 0.0F)
        menu_cap_ratio_ = 100.0F / static_cast<float>(font_->pixel_size_for_cap_height(100));
    MenuLetter letter;
    letter.cap = std::max(cap, 1.0F);
    letter.pixels = std::clamp(static_cast<int>(std::lround(letter.cap / menu_cap_ratio_)), 8, 256);
    // UI area: a mod theme's TrueType face for this family (app_mod_ui.inc).
    if (mod_ui_theme_font(face, letter))
        return letter;
    RetailFonts *fonts = menu_fonts();
    if (fonts == nullptr)
        return letter;
    static constexpr std::array<RetailFont, 3> classic_display{
        RetailFont::small, RetailFont::medium, RetailFont::large};
    static constexpr std::array<RetailFont, 4> classic_text{
        RetailFont::myriad_small, RetailFont::myriad_medium, RetailFont::medium_large,
        RetailFont::myriad_large};
    // 1.9.9.8's SmallFont is left out: its bitmaps space some letters apart
    // ("w orld") in running text, where the originals never use it; small
    // lettering is MedFont reduced instead.
    static constexpr std::array<RetailFont, 3> modern_any{
        RetailFont::medium, RetailFont::medium_large, RetailFont::large};
    const std::span<const RetailFont> family =
        menu_generation() == RetailGeneration::modern
            ? std::span<const RetailFont>{modern_any}
            : face == MenuFace::display ? std::span<const RetailFont>{classic_display}
                                        : std::span<const RetailFont>{classic_text};
    std::array<int, 4> caps{};
    for (std::size_t index = 0U; index < family.size(); ++index)
        if (const SpriteFont *member = fonts->font(family[index]))
            caps[index] = member->cap_height();
    const ui::MenuLetterChoice choice =
        ui::choose_menu_letter(std::span<const int>{caps.data(), family.size()}, letter.cap);
    if (choice.member >= family.size())
        return letter;
    const SpriteFont *chosen = fonts->font(family[choice.member]);
    if (chosen == nullptr || !chosen->covers(value))
        return letter;
    letter.fonts = fonts;
    letter.font = family[choice.member];
    letter.scale = choice.scale;
    letter.cap = static_cast<float>(chosen->cap_height()) * letter.scale;
    return letter;
}

[[nodiscard]] float menu_width(std::string_view value, float cap, MenuFace face = MenuFace::display)
{
    value = mod_ui_game_text(value); // UI area: a mod theme's word for it
    std::string plain;
    if (value.find('\xE2') != std::string_view::npos || value.find('\xC2') != std::string_view::npos)
    {
        plain = ui::menu_plain_punctuation(value);
        value = plain;
    }
    const MenuLetter letter = menu_letter(face, cap, value);
    if (letter.fonts != nullptr)
        return letter.fonts->measure(letter.font, value, letter.scale).width;
    return (letter.ttf != nullptr ? letter.ttf : font_.get())->measure(value, letter.pixels).width;
}

// The distance from one line's capitals to the next line's, for text of that
// capital height (LargeFont's own line is 58 for 36, MedLargeFont's 39 for 23).
[[nodiscard]] static float menu_line(float cap) noexcept
{
    return cap * 1.66F;
}

// How thick the outline of lettering that tall is: two units from MedLargeFont
// up, one below it, as the originals draw theirs.
[[nodiscard]] float menu_outline(float cap) const noexcept
{
    const float unit = menu_unit();
    return std::max(1.0F, std::round((cap / unit < 20.0F ? 1.0F : 2.0F) * unit));
}

// Draws `value` with the TOP OF ITS CAPITALS at `cap_top`. `x` is the left
// edge, the centre or the right edge according to `align`.
void menu_label(std::string_view value, float x, float cap_top, float cap, SDL_Color color,
                TextAlign align = TextAlign::left, MenuFace face = MenuFace::display,
                bool outlined = true)
{
    if (value.empty() || color.a == 0U)
        return;
    value = mod_ui_game_text(value); // UI area: a mod theme's word for it
    // Typographic punctuation the shipped fonts lack, written the plain way.
    std::string plain;
    if (value.find('\xE2') != std::string_view::npos || value.find('\xC2') != std::string_view::npos)
    {
        plain = ui::menu_plain_punctuation(value);
        value = plain;
    }
    const MenuLetter letter = menu_letter(face, cap, value);
    if (letter.fonts == nullptr)
    {
        // The UI font (or a mod theme's face): its draw() takes the top of
        // the text cell.
        FontRenderer &ui_font = letter.ttf != nullptr ? *letter.ttf : *font_;
        const float top = cap_top - (static_cast<float>(ui_font.baseline(letter.pixels)) - letter.cap);
        ui_font.draw(value, std::round(x), std::round(top), letter.pixels, color, align, outlined);
        return;
    }
    const SpriteFont &font = *letter.fonts->font(letter.font);
    const float width = letter.fonts->measure(letter.font, value, letter.scale).width;
    float left = align == TextAlign::center ? x - width * 0.5F
                 : align == TextAlign::right ? x - width
                                             : x;
    float top = cap_top - static_cast<float>(font.cap_top()) * letter.scale;
    // Unscaled bitmaps belong on whole pixels.
    left = std::round(left);
    top = std::round(top);
    letter.fonts->set_sharp_magnification(true);
    if (outlined)
    {
        // DrawingTools.DrawOutlinedText: four diagonal copies, then the text.
        const float offset = menu_outline(letter.cap);
        const SDL_Color ink{0U, 0U, 0U, color.a};
        letter.fonts->draw(letter.font, value, left + offset, top + offset, ink, letter.scale);
        letter.fonts->draw(letter.font, value, left - offset, top - offset, ink, letter.scale);
        letter.fonts->draw(letter.font, value, left - offset, top + offset, ink, letter.scale);
        letter.fonts->draw(letter.font, value, left + offset, top - offset, ink, letter.scale);
    }
    letter.fonts->draw(letter.font, value, left, top, color, letter.scale);
    letter.fonts->set_sharp_magnification(false);
}

// `value` shortened with an ellipsis until it is no wider than `width`.
[[nodiscard]] std::string menu_fitted(std::string value, float width, float cap, MenuFace face)
{
    // UI area: shorten the word a mod theme shows, not the one it replaced.
    if (const std::string_view themed = mod_ui_game_text(value); themed.data() != value.data())
        value = std::string{themed};
    if (menu_width(value, cap, face) <= width)
        return value;
    while (!value.empty())
    {
        std::size_t last = value.size() - 1U;
        while (last > 0U && (static_cast<unsigned char>(value[last]) & 0xC0U) == 0x80U)
            --last;
        value.resize(last);
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
        if (menu_width(value + "...", cap, face) <= width)
            return value + "...";
    }
    return {};
}

// One line inside `bounds`: capitals centred on its height, shortened to its
// width. Returns the width drawn.
float menu_label_in(std::string_view value, Rect bounds, float cap, SDL_Color color,
                    TextAlign align = TextAlign::left, MenuFace face = MenuFace::display,
                    bool outlined = true)
{
    const std::string fitted = menu_fitted(std::string{value}, bounds.width, cap, face);
    if (fitted.empty())
        return 0.0F;
    const float x = align == TextAlign::center ? bounds.x + bounds.width * 0.5F
                    : align == TextAlign::right ? bounds.x + bounds.width
                                                : bounds.x;
    menu_label(fitted, x, bounds.y + (bounds.height - cap) * 0.5F, cap, color, align, face,
               outlined);
    return menu_width(fitted, cap, face);
}

// `value` broken at spaces into lines no wider than `width`.
[[nodiscard]] std::vector<std::string> menu_wrapped(std::string_view value, float width, float cap,
                                                    MenuFace face)
{
    value = mod_ui_game_text(value); // UI area: a mod theme's words, whole
    std::vector<std::string> lines;
    std::string line;
    std::size_t start = 0U;
    while (start <= value.size())
    {
        const std::size_t newline = std::min(value.find('\n', start), value.size());
        std::string_view paragraph = value.substr(start, newline - start);
        while (!paragraph.empty())
        {
            const std::size_t space = paragraph.find(' ');
            const std::string_view word =
                paragraph.substr(0U, space == std::string_view::npos ? paragraph.size() : space);
            paragraph.remove_prefix(space == std::string_view::npos ? paragraph.size()
                                                                    : space + 1U);
            if (word.empty())
                continue;
            std::string candidate = line.empty() ? std::string{word} : line + ' ' + std::string{word};
            if (!line.empty() && menu_width(candidate, cap, face) > width)
            {
                lines.push_back(std::move(line));
                candidate = std::string{word};
            }
            line = std::move(candidate);
        }
        if (!line.empty())
            lines.push_back(std::move(line));
        line.clear();
        start = newline + 1U;
    }
    return lines;
}

// A paragraph from the top of `bounds`, as many lines as fit its height (the
// last one shortened if text remains). Returns the height used.
float menu_paragraph(std::string_view value, Rect bounds, float cap, SDL_Color color,
                     MenuFace face = MenuFace::text, TextAlign align = TextAlign::left)
{
    const float pitch = menu_line(cap);
    const std::vector<std::string> lines = menu_wrapped(value, bounds.width, cap, face);
    const std::size_t room = bounds.height <= 0.0F
                                 ? lines.size()
                                 : static_cast<std::size_t>(
                                       std::max(1.0F, std::floor((bounds.height - cap) / pitch) + 1.0F));
    const float x = align == TextAlign::center ? bounds.x + bounds.width * 0.5F
                    : align == TextAlign::right ? bounds.x + bounds.width
                                                : bounds.x;
    float y = bounds.y;
    for (std::size_t index = 0U; index < lines.size() && index < room; ++index)
    {
        const bool cut = index + 1U == room && index + 1U < lines.size();
        menu_label(cut ? menu_fitted(lines[index] + " ...", bounds.width, cap, face)
                       : lines[index],
                   x, y, cap, color, align, face);
        y += pitch;
    }
    return y - bounds.y - (pitch - cap);
}

// ------------------------------------------------------------- the backdrop
//
// The picture behind the front end is a still of the origin tower rendered
// from the player's own content pack by tools/Capture-MenuBackground.ps1 and
// kept in their local data folder (asset_paths.hpp: menu_background_path()).
// It is never part of the repository. Without one the built-in backdrop is
// drawn, exactly as before.

struct MenuBackdropPixels final
{
    SDL_Surface *picture{};
    SDL_Surface *soft{}; // a small blurred copy, for the plates behind text
};

// Decodes the picture and makes its blurred copy. Runs on a worker thread so
// the first frame does not wait for a 4K image.
[[nodiscard]] static MenuBackdropPixels load_menu_backdrop_pixels(std::filesystem::path path)
{
    MenuBackdropPixels pixels;
    SDL_Surface *loaded = SDL_LoadSurface(path.string().c_str());
    if (loaded == nullptr)
        return pixels;
    pixels.picture = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (pixels.picture == nullptr)
        return pixels;
    // Reduce by whole-pixel averaging to about 320 wide, then blur.
    const int width = std::clamp(pixels.picture->w / std::max(1, pixels.picture->w / 320), 16, 640);
    const int height = std::max(
        9, static_cast<int>(std::lround(static_cast<double>(width) * pixels.picture->h /
                                        std::max(1, pixels.picture->w))));
    std::vector<float> small(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
    const auto *source = static_cast<const std::uint8_t *>(pixels.picture->pixels);
    for (int y = 0; y < height; ++y)
    {
        const int y0 = y * pixels.picture->h / height;
        const int y1 = std::max(y0 + 1, (y + 1) * pixels.picture->h / height);
        for (int x = 0; x < width; ++x)
        {
            const int x0 = x * pixels.picture->w / width;
            const int x1 = std::max(x0 + 1, (x + 1) * pixels.picture->w / width);
            std::array<double, 3> sum{};
            for (int row = y0; row < y1; ++row)
            {
                const std::uint8_t *line = source + row * pixels.picture->pitch + x0 * 4;
                for (int column = x0; column < x1; ++column, line += 4)
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                        sum[channel] += line[channel];
            }
            const double count = static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                small[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)) *
                          3U +
                      channel] = static_cast<float>(sum[channel] / count);
        }
    }
    // Three box passes each way are close to a Gaussian.
    const int radius = std::max(2, width / 64);
    std::vector<float> scratch(small.size());
    const auto blur = [&](bool horizontal) {
        const int outer = horizontal ? height : width;
        const int inner = horizontal ? width : height;
        for (int line = 0; line < outer; ++line)
            for (int position = 0; position < inner; ++position)
            {
                std::array<float, 3> sum{};
                int count = 0;
                for (int offset = -radius; offset <= radius; ++offset)
                {
                    const int sample = std::clamp(position + offset, 0, inner - 1);
                    const std::size_t index =
                        (horizontal ? static_cast<std::size_t>(line) * static_cast<std::size_t>(width) +
                                          static_cast<std::size_t>(sample)
                                    : static_cast<std::size_t>(sample) * static_cast<std::size_t>(width) +
                                          static_cast<std::size_t>(line)) *
                        3U;
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                        sum[channel] += small[index + channel];
                    ++count;
                }
                const std::size_t target =
                    (horizontal ? static_cast<std::size_t>(line) * static_cast<std::size_t>(width) +
                                      static_cast<std::size_t>(position)
                                : static_cast<std::size_t>(position) * static_cast<std::size_t>(width) +
                                      static_cast<std::size_t>(line)) *
                    3U;
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    scratch[target + channel] = sum[channel] / static_cast<float>(count);
            }
        small.swap(scratch);
    };
    for (int pass = 0; pass < 3; ++pass)
    {
        blur(true);
        blur(false);
    }
    pixels.soft = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (pixels.soft != nullptr)
        for (int y = 0; y < height; ++y)
        {
            auto *line = static_cast<std::uint8_t *>(pixels.soft->pixels) + y * pixels.soft->pitch;
            for (int x = 0; x < width; ++x)
            {
                const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                           static_cast<std::size_t>(x)) *
                                          3U;
                line[x * 4 + 0] = static_cast<std::uint8_t>(std::lround(small[index]));
                line[x * 4 + 1] = static_cast<std::uint8_t>(std::lround(small[index + 1U]));
                line[x * 4 + 2] = static_cast<std::uint8_t>(std::lround(small[index + 2U]));
                line[x * 4 + 3] = 255U;
            }
        }
    return pixels;
}

// A 16:9 picture cropped to a 32:9 window keeps half its height, so the
// capture script renders every shot a second time at 32:9. A window at least
// this many times as wide as it is tall is shown that one.
[[nodiscard]] static bool menu_backdrop_is_wide(int width, int height) noexcept
{
    return height > 0 && static_cast<float>(width) / static_cast<float>(height) >= 2.6F;
}

// The picture file for a window of that shape; empty when there is none.
[[nodiscard]] std::filesystem::path menu_backdrop_file(bool wide) const
{
    if (!options_.menu_backdrop.has_value())
        return menu_background_path(wide);
    // --menu-backdrop PATH: that picture, and beside it PATH with "-wide"
    // before the extension for a wide window, when there is one.
    const std::filesystem::path &path = *options_.menu_backdrop;
    if (wide)
    {
        std::filesystem::path wide_path = path;
        wide_path.replace_filename(path.stem().string() + "-wide" + path.extension().string());
        std::error_code error;
        if (std::filesystem::is_regular_file(wide_path, error))
            return wide_path;
    }
    return path;
}

void load_menu_backdrop(const std::filesystem::path &path, bool announce)
{
    menu_backdrop_path_ = path;
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error))
    {
        if (announce)
            std::cout << "Menu background: built-in (no picture at "
                      << (path.empty() ? menu_background_directory().string() : path.string())
                      << "; tools/Capture-MenuBackground.ps1 makes one)\n";
        return;
    }
    std::cout << "Menu background: " << path.string() << '\n';
    menu_backdrop_loading_ = std::async(std::launch::async, load_menu_backdrop_pixels, path);
}

// Starts loading the player's menu picture, if they have one. Called once the
// window has the shape it will be shown at.
void begin_menu_backdrop()
{
    int width{};
    int height{};
    SDL_GetRenderOutputSize(renderer_, &width, &height);
    menu_backdrop_begun_ = true;
    menu_backdrop_wide_ = menu_backdrop_is_wide(width, height);
    load_menu_backdrop(menu_backdrop_file(menu_backdrop_wide_), true);
}

// Takes the decoded picture from the worker once it is ready.
void adopt_menu_backdrop(bool wait)
{
    // A window that has changed shape enough to want the other render.
    if (menu_backdrop_begun_ && !menu_backdrop_loading_.valid() &&
        menu_backdrop_is_wide(metrics_.output_width, metrics_.output_height) != menu_backdrop_wide_)
    {
        menu_backdrop_wide_ = !menu_backdrop_wide_;
        if (const std::filesystem::path wanted = menu_backdrop_file(menu_backdrop_wide_);
            wanted != menu_backdrop_path_)
            load_menu_backdrop(wanted, false);
    }
    if (!menu_backdrop_loading_.valid())
        return;
    if (!wait && menu_backdrop_loading_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        return;
    const bool first = menu_backdrop_ == nullptr;
    MenuBackdropPixels pixels = menu_backdrop_loading_.get();
    const auto upload = [this](SDL_Surface *surface) -> std::shared_ptr<SDL_Texture> {
        if (surface == nullptr)
            return {};
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_DestroySurface(surface);
        if (texture == nullptr)
            return {};
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        return {texture, SDL_DestroyTexture};
    };
    std::shared_ptr<SDL_Texture> picture = upload(pixels.picture);
    std::shared_ptr<SDL_Texture> soft = upload(pixels.soft);
    if (picture == nullptr)
    {
        // Whatever was showing (another render, or nothing) stays.
        std::cerr << "Menu background: " << menu_backdrop_path_.string()
                  << " could not be read; "
                  << (first ? "using the built-in backdrop.\n" : "keeping the picture shown.\n");
        return;
    }
    menu_backdrop_ = std::move(picture);
    menu_backdrop_soft_ = std::move(soft);
    // Fades in over the built-in backdrop; a change of render just swaps.
    if (first)
        menu_backdrop_since_ = elapsed_;
}

// Before the renderer goes: the textures, and a decode still in flight.
void release_menu_backdrop()
{
    if (menu_backdrop_loading_.valid())
    {
        const MenuBackdropPixels pixels = menu_backdrop_loading_.get();
        SDL_DestroySurface(pixels.picture);
        SDL_DestroySurface(pixels.soft);
    }
    menu_backdrop_.reset();
    menu_backdrop_soft_.reset();
}

// The part of a `width` x `height` picture that covers the window without
// stretching: scaled until both sides are filled, the overflow cropped evenly.
[[nodiscard]] SDL_FRect menu_backdrop_source(float width, float height) const noexcept
{
    const ui::PictureCrop crop =
        ui::cover_crop(width, height, static_cast<float>(std::max(metrics_.output_width, 1)),
                       static_cast<float>(std::max(metrics_.output_height, 1)));
    return {crop.x, crop.y, crop.width, crop.height};
}

// A vertical or horizontal run from one black opacity to another.
void menu_shade(Rect rect, float first, float last, bool horizontal = false)
{
    const SDL_FColor a{0.0F, 0.0F, 0.0F, std::clamp(first, 0.0F, 1.0F)};
    const SDL_FColor b{0.0F, 0.0F, 0.0F, std::clamp(last, 0.0F, 1.0F)};
    const std::array<SDL_Vertex, 4> vertices{
        {{{rect.x, rect.y}, a, {}},
         {{rect.x + rect.width, rect.y}, horizontal ? b : a, {}},
         {{rect.x + rect.width, rect.y + rect.height}, b, {}},
         {{rect.x, rect.y + rect.height}, horizontal ? a : b, {}}}};
    constexpr std::array indices{0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

// How much of the picture is showing: nothing until it has been decoded, then
// a short fade in over the built-in backdrop.
[[nodiscard]] float menu_backdrop_shown()
{
    adopt_menu_backdrop(options_.screenshot_path.has_value());
    if (menu_backdrop_ == nullptr)
        return 0.0F;
    if (options_.screenshot_path.has_value() || settings_.reduced_motion)
        return 1.0F;
    return smoothstep(std::clamp((elapsed_ - menu_backdrop_since_) / 0.45F, 0.0F, 1.0F));
}

// The picture, covering the window, with the light taken down where the
// header and the footer hints sit. Draws nothing when there is no picture.
void draw_menu_backdrop()
{
    const float shown = menu_backdrop_shown();
    if (shown <= 0.0F)
        return;
    float width{};
    float height{};
    SDL_GetTextureSize(menu_backdrop_.get(), &width, &height);
    if (width <= 0.0F || height <= 0.0F)
        return;
    const float window_width = static_cast<float>(metrics_.output_width);
    const float window_height = static_cast<float>(metrics_.output_height);
    const SDL_FRect source = menu_backdrop_source(width, height);
    const SDL_FRect target{0.0F, 0.0F, window_width, window_height};
    SDL_SetTextureAlphaMod(menu_backdrop_.get(), static_cast<Uint8>(std::lround(shown * 255.0F)));
    SDL_RenderTexture(renderer_, menu_backdrop_.get(), &source, &target);
    // A little off the whole picture so white lettering always has contrast,
    // more behind the header and the hints.
    const float overall = settings_.high_contrast ? 0.42F : 0.16F;
    fill({0.0F, 0.0F, window_width, window_height},
         {0U, 0U, 0U, static_cast<Uint8>(std::lround(overall * shown * 255.0F))});
    menu_shade({0.0F, 0.0F, window_width, window_height * 0.24F}, 0.50F * shown, 0.0F);
    menu_shade({0.0F, window_height * 0.80F, window_width, window_height * 0.20F}, 0.0F,
               0.62F * shown);
}

// A plate for text that needs more than an outline (tables, long lists):
// the blurred picture under it, darkened. Over the live world (the in-game
// screens) there is no picture and the plate is the originals' black wash.
void menu_plate(Rect rect, float darkness = 0.58F)
{
    if (rect.width <= 0.0F || rect.height <= 0.0F)
        return;
    // UI area: a mod theme's panel image in place of the darkened backdrop.
    if (mod_ui_plate(rect, modding::ui::ImageKey::panel))
        return;
    if (settings_.high_contrast)
        darkness = std::max(darkness, 0.86F);
    const bool front_end = menu_.screen() != ui::MenuScreen::world_preview && !in_game_menu_screen();
    if (front_end && menu_backdrop_soft_ != nullptr && menu_backdrop_ != nullptr)
    {
        float width{};
        float height{};
        SDL_GetTextureSize(menu_backdrop_soft_.get(), &width, &height);
        const SDL_FRect whole = menu_backdrop_source(width, height);
        const float window_width = static_cast<float>(std::max(metrics_.output_width, 1));
        const float window_height = static_cast<float>(std::max(metrics_.output_height, 1));
        const SDL_FRect source{whole.x + rect.x / window_width * whole.w,
                               whole.y + rect.y / window_height * whole.h,
                               rect.width / window_width * whole.w,
                               rect.height / window_height * whole.h};
        const SDL_FRect target{rect.x, rect.y, rect.width, rect.height};
        SDL_RenderTexture(renderer_, menu_backdrop_soft_.get(), &source, &target);
    }
    fill(rect, {0U, 0U, 0U, static_cast<Uint8>(std::lround(std::clamp(darkness, 0.0F, 1.0F) * 255.0F))});
}

// The band under the focused row of a table or list.
void menu_focus_band(Rect rect)
{
    fill(rect, {255U, 255U, 255U, settings_.high_contrast ? Uint8{56U} : Uint8{30U}});
}

// A thin rule in the row colour.
void menu_rule(float x, float y, float width, float opacity = 0.35F)
{
    const float thickness = std::max(1.0F, std::round(menu_unit()));
    fill({x, y, width, thickness},
         {255U, 255U, 255U, static_cast<Uint8>(std::lround(std::clamp(opacity, 0.0F, 1.0F) * 255.0F))});
}

// ----------------------------------------------------------------- sounds
//
// MenuScreen plays its SelectSound when the focus moves (keys, pad, or the
// pointer coming to rest on another row) and its ClickSound when a row is
// chosen or the screen is left; every menu sets both to "Click". Dialogs play
// "Popup" as they open. Called once a frame.
void service_menu_sounds()
{
    const ui::MenuScreen screen = menu_.screen();
    const std::size_t selection = menu_.selection();
    const ui::SettingsTab tab = menu_.settings_tab();
    const std::size_t serial = menu_.transition_serial();
    const bool dialog = host_text_field_ != HostTextField::none || locator_name_editing_ ||
                        display_prompt_active() || screen == ui::MenuScreen::exit_confirmation ||
                        addon_world_prompt_.has_value();
    if (menu_sound_primed_ && sound_bank_ != nullptr && !options_.screenshot_path.has_value())
    {
        const bool in_world = screen == ui::MenuScreen::world_preview;
        if (dialog && !menu_sound_dialog_)
            mod_ui_menu_cue("Popup"); // UI area: or a mod theme's sound
        else if (serial != menu_sound_serial_ || screen != menu_sound_screen_)
        {
            // Entering the in-game menu from the world is silent in the
            // originals; everything else is a choice or a way back.
            if (!(menu_sound_screen_ == ui::MenuScreen::world_preview &&
                  screen == ui::MenuScreen::pause))
                mod_ui_menu_cue("Click");
        }
        else if (!in_world &&
                 ((menu_focus_moved_by_input_ && selection != menu_sound_selection_) ||
                  tab != menu_sound_tab_))
            mod_ui_menu_cue("Click");
    }
    menu_focus_moved_by_input_ = false;
    if (!menu_sound_primed_ || selection != menu_sound_selection_ || screen != menu_sound_screen_ ||
        tab != menu_sound_tab_)
        menu_focus_since_ = elapsed_;
    menu_sound_primed_ = true;
    menu_sound_screen_ = screen;
    menu_sound_selection_ = selection;
    menu_sound_tab_ = tab;
    menu_sound_serial_ = serial;
    menu_sound_dialog_ = dialog;
}

// -------------------------------------------------------------- the title
//
// "OpenMinerZ" set the way the CastleMiner Z title is: leaning steel capitals
// in the menu's own display face with a black outline, and the Z large and red
// behind the end of the word. It is lettering, not the retail logo artwork,
// which is not OpenMinerZ's to present as its own.
void draw_menu_title(float center_x, float top, float cap)
{
    constexpr std::string_view word{"OpenMiner"};
    constexpr std::string_view mark{"Z"};
    RetailFonts *fonts = menu_fonts();
    const SpriteFont *face = fonts != nullptr ? fonts->font(RetailFont::large) : nullptr;
    if (face == nullptr || face->cap_height() <= 0)
    {
        // The UI font: the same arrangement, upright.
        const int word_pixels = std::clamp(static_cast<int>(std::lround(cap / 0.70F)), 8, 256);
        const int mark_pixels = std::clamp(static_cast<int>(std::lround(cap * 1.5F / 0.70F)), 8, 256);
        const float word_width = font_->measure(word, word_pixels).width;
        const float mark_width = font_->measure(mark, mark_pixels).width;
        const float left = center_x - (word_width + mark_width) * 0.5F;
        font_->draw(word, left, top - (static_cast<float>(font_->baseline(word_pixels)) - cap),
                    word_pixels, {255U, 255U, 255U, 255U}, TextAlign::left, true);
        font_->draw(mark, left + word_width,
                    top - cap * 0.25F - (static_cast<float>(font_->baseline(mark_pixels)) - cap * 1.5F),
                    mark_pixels, menu_generation() == RetailGeneration::modern
                                     ? SDL_Color{78U, 177U, 61U, 255U}
                                     : SDL_Color{226U, 24U, 24U, 255U},
                    TextAlign::left, true);
        return;
    }
    const float scale = cap / static_cast<float>(face->cap_height());
    const float mark_scale = scale * 1.72F;
    const float slant = 0.19F;
    const float word_width = fonts->measure(RetailFont::large, word, scale).width;
    const float mark_width = fonts->measure(RetailFont::large, mark, mark_scale).width;
    // The Z tucks in behind the last letters.
    const float overlap = mark_width * 0.26F;
    const float left = std::round(center_x - (word_width + mark_width - overlap) * 0.5F);
    const float word_top = std::round(top - static_cast<float>(face->cap_top()) * scale);
    // Its capitals rise above the word's and hang below its baseline.
    const float mark_cap_top = top - cap * 0.40F;
    const float mark_top = std::round(mark_cap_top - static_cast<float>(face->cap_top()) * mark_scale);
    const float mark_left = left + word_width - overlap;
    const SDL_Color ink{0U, 0U, 0U, 255U};
    const auto stamp = [&](std::string_view text, float x, float y, float size, SDL_Color upper,
                           SDL_Color lower, float edge) {
        constexpr std::array<std::array<float, 2>, 8> around{
            {{-1.0F, -1.0F}, {0.0F, -1.0F}, {1.0F, -1.0F}, {-1.0F, 0.0F},
             {1.0F, 0.0F}, {-1.0F, 1.0F}, {0.0F, 1.0F}, {1.0F, 1.0F}}};
        // A soft drop of shadow, the black edge, then the face.
        fonts->draw_slanted(RetailFont::large, text, x + edge * 1.6F, y + edge * 2.2F,
                            {0U, 0U, 0U, 150U}, {0U, 0U, 0U, 150U}, size, slant);
        for (const auto &direction : around)
            fonts->draw_slanted(RetailFont::large, text, x + direction[0] * edge,
                                y + direction[1] * edge, ink, ink, size, slant);
        fonts->draw_slanted(RetailFont::large, text, x, y, upper, lower, size, slant);
    };
    const float edge = std::max(1.0F, std::round(cap * 0.055F));
    fonts->set_sharp_magnification(true);
    if (menu_generation() == RetailGeneration::modern)
        stamp(mark, mark_left, mark_top, mark_scale, {120U, 214U, 92U, 255U}, {38U, 110U, 30U, 255U},
              edge);
    else
        stamp(mark, mark_left, mark_top, mark_scale, {244U, 34U, 30U, 255U}, {126U, 0U, 0U, 255U},
              edge);
    stamp(word, left, word_top, scale, {255U, 255U, 255U, 255U}, {150U, 158U, 170U, 255U}, edge);
    fonts->set_sharp_magnification(false);
}

// What the keys do, written the way the Remastered inventory writes it: the
// key in a thin box, then the action. `x` is the left edge, the centre or the
// right edge of the whole line; hints that would run past `room` are left off.
struct MenuKeyHint final
{
    std::string_view key;
    std::string_view action;
};

void menu_key_hints(std::span<const MenuKeyHint> hints, float x, float cap_top, float room,
                    TextAlign align = TextAlign::left)
{
    if (hints.empty())
        return;
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const float cap = menu_cap(menu_cap_small);
    const float inset = 5.0F * unit;
    const float gap = 6.0F * unit;
    const float between = 16.0F * unit;
    // Lay the line out first so it can be centred and cut where it must be.
    struct Piece final
    {
        float key_width{};
        float action_width{};
    };
    std::vector<Piece> pieces;
    float total = 0.0F;
    for (const MenuKeyHint &hint : hints)
    {
        const Piece piece{menu_width(hint.key, cap, MenuFace::text) + inset * 2.0F,
                          menu_width(hint.action, cap, MenuFace::text)};
        const float width = piece.key_width + gap + piece.action_width;
        if (total + width + (pieces.empty() ? 0.0F : between) > room)
            break;
        total += width + (pieces.empty() ? 0.0F : between);
        pieces.push_back(piece);
    }
    float left = align == TextAlign::center ? x - total * 0.5F
                 : align == TextAlign::right ? x - total
                                             : x;
    const float box_top = std::round(cap_top - 5.0F * unit);
    const float box_height = std::round(cap + 10.0F * unit);
    for (std::size_t index = 0U; index < pieces.size(); ++index)
    {
        const Rect box{std::round(left), box_top, std::round(pieces[index].key_width), box_height};
        fill(box, {0U, 0U, 0U, 120U});
        stroke(box, {255U, 255U, 255U, 170U}, std::max(1.0F, std::round(unit)));
        menu_label(hints[index].key, box.x + inset, cap_top, cap, ink.text, TextAlign::left,
                   MenuFace::text);
        menu_label(hints[index].action, box.x + box.width + gap, cap_top, cap, ink.quiet,
                   TextAlign::left, MenuFace::text);
        left += pieces[index].key_width + gap + pieces[index].action_width + between;
    }
}

// The frame every front-end screen shares: who is playing, in the top-left
// corner where the originals draw the gamertag (MyriadMedium, one-pixel
// outline), and what the keys do along the bottom.
void draw_menu_chrome()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    const std::string_view profile = steam_identity_verified_ && !online_persona_name_.empty()
                                         ? std::string_view{online_persona_name_}
                                         : std::string_view{profile_name_};
    const float name_cap = menu_cap(menu_cap_medium);
    const float note_cap = menu_cap(menu_cap_small);
    menu_label(menu_fitted(std::string{profile}, safe.width * 0.26F, name_cap, MenuFace::text),
               safe.x + 12.0F * unit, safe.y + 10.0F * unit, name_cap, ink.text, TextAlign::left,
               MenuFace::text);
    menu_label(steam_identity_verified_ ? "Steam profile" : "Local profile", safe.x + 12.0F * unit,
               safe.y + 10.0F * unit + name_cap + 9.0F * unit, note_cap, ink.quiet, TextAlign::left,
               MenuFace::text);

    const bool pad = gamepad_input_active_;
    const ui::MenuScreen screen = menu_.screen();
    std::vector<MenuKeyHint> hints;
    if (online_join_stage_ != OnlineJoinStage::idle)
        hints = {{pad ? "B" : "Esc", "Cancel"}};
    else if (waiting_for_world_)
        hints = {}; // nothing to press while a world loads
    else if (screen == ui::MenuScreen::settings)
        hints = {{"Up / Down", "Select"},
                 {"Left / Right", "Change"},
                 {pad ? "LB / RB" : "Q / E", "Page"},
                 {pad ? "Y" : "R", "Reset Page"},
                 {pad ? "B" : "Esc", "Save & Back"}};
    else if (screen == ui::MenuScreen::world_select)
        hints = pad ? std::vector<MenuKeyHint>{{"Up / Down", "Select"}, {"A", "Open"}, {"B", "Back"}}
                    : std::vector<MenuKeyHint>{{"Up / Down", "Select"},
                                               {"Left / Right", "Page"},
                                               {"F", "Sort"},
                                               {"Enter", "Open"},
                                               {"Esc", "Back"}};
    else if (screen == ui::MenuScreen::host_details || screen == ui::MenuScreen::host_access ||
             screen == ui::MenuScreen::host_gameplay)
        hints = {{"Up / Down", "Select"}, {"Left / Right", "Change"}, {pad ? "B" : "Esc", "Back"}};
    // (Add-Ons) The world prompt's buttons, and the add-on rows' switch.
    else if (addon_world_prompt_.has_value())
        hints = {{"Left / Right", "Select"}, {pad ? "A" : "Enter", "Choose"}, {pad ? "B" : "Esc", "Cancel"}};
    else if (screen == ui::MenuScreen::addons && selected_addon() != nullptr)
        hints = {{"Up / Down", "Select"},
                 {pad ? "A" : "Enter", "Turn On / Off"},
                 {"Left / Right", "Off / On"},
                 {pad ? "B" : "Esc", "Back"}};
    else
        hints = {{"Up / Down", "Select"}, {pad ? "A" : "Enter", "Choose"}, {pad ? "B" : "Esc", "Back"}};
    menu_key_hints(hints, safe.x + 12.0F * unit, safe.y + safe.height - 12.0F * unit - note_cap,
                   safe.width - 24.0F * unit, TextAlign::left);
}

// A screen's heading: one line of the display face centred at the top, where
// GameModeMenu draws "Choose a Game Mode". Returns where the content below
// it may start.
float draw_menu_heading(std::string_view title)
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    // Room is left for the player name on the left.
    const float room = safe.width * 0.46F;
    float cap = menu_cap(menu_cap_large);
    const float width = menu_width(title, cap, MenuFace::display);
    if (width > room)
        cap *= room / width;
    const float top = safe.y + 14.0F * unit + (menu_cap(menu_cap_large) - cap) * 0.5F;
    menu_label(title, safe.x + safe.width * 0.5F, top, cap, ink.heading, TextAlign::center,
               MenuFace::display);
    return safe.y + 14.0F * unit + menu_cap(menu_cap_large) + 30.0F * unit;
}

// The words beside a focused row. Game modes and difficulty levels use the
// paragraphs the shipped GameModeMenu and DifficultyLevelScreen show.
[[nodiscard]] std::string_view menu_entry_description(const ui::MenuEntry &entry) const noexcept
{
    if (!entry.enabled)
        return entry.description;
    const RetailGeneration generation = menu_generation();
    std::string_view retail_words;
    switch (entry.command)
    {
    case ui::MenuCommand::select_mode_endurance:
        retail_words = retail_game_mode_description(GameMode::endurance, generation);
        break;
    case ui::MenuCommand::select_mode_survival:
        retail_words = retail_game_mode_description(GameMode::survival, generation);
        break;
    case ui::MenuCommand::select_mode_dragon_endurance:
        retail_words = retail_game_mode_description(GameMode::dragon_endurance, generation);
        break;
    case ui::MenuCommand::select_mode_creative:
        retail_words = retail_game_mode_description(GameMode::creative, generation);
        break;
    case ui::MenuCommand::select_mode_exploration:
        retail_words = retail_game_mode_description(GameMode::exploration, generation);
        break;
    case ui::MenuCommand::select_mode_scavenger:
        retail_words = retail_game_mode_description(GameMode::scavenger, generation);
        break;
    case ui::MenuCommand::select_difficulty_no_enemies:
        retail_words = retail_difficulty_description(GameDifficulty::no_enemies, generation);
        break;
    case ui::MenuCommand::select_difficulty_easy:
        retail_words = retail_difficulty_description(GameDifficulty::easy, generation);
        break;
    case ui::MenuCommand::select_difficulty_normal:
        retail_words = retail_difficulty_description(GameDifficulty::hard, generation);
        break;
    case ui::MenuCommand::select_difficulty_hardcore:
        retail_words = retail_difficulty_description(GameDifficulty::hardcore, generation);
        break;
    default:
        break;
    }
    return retail_words.empty() ? entry.description : retail_words;
}

// A menu as the originals lay one out: rows in the display face right-aligned
// against a line 125 units left of centre, starting 175 units down, 54 apart;
// the focused row's description to the right of centre, and under it `note`,
// a quiet line of the screen's own. A list too long for the screen is set
// smaller, and scrolls with the focus if that is not enough.
void draw_menu_rows(float list_top, std::string_view note = {})
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    const auto &entries = menu_.entries();
    if (entries.empty())
        return;
    const float center = safe.x + safe.width * 0.5F;
    const float bottom = safe.y + safe.height - 40.0F * unit;
    float cap = menu_cap(menu_cap_large);
    float pitch = menu_cap(54.0F);
    const float available = std::max(pitch, bottom - list_top);
    std::size_t visible = entries.size();
    std::size_t first = 0U;
    if (pitch * static_cast<float>(entries.size()) > available)
    {
        const float fit =
            std::max(0.60F, available / (pitch * static_cast<float>(entries.size())));
        cap *= fit;
        pitch *= fit;
        visible = std::clamp<std::size_t>(static_cast<std::size_t>(available / pitch), 1U,
                                          entries.size());
        if (menu_.selection() >= visible)
            first = std::min(menu_.selection() - visible + 1U, entries.size() - visible);
    }
    const std::size_t selection = std::min(menu_.selection(), entries.size() - 1U);
    const float right = center - 125.0F * unit;
    const float reach = std::max(120.0F * unit, right - safe.x - 8.0F * unit);
    for (std::size_t index = first; index < first + visible; ++index)
    {
        const ui::MenuEntry &entry = entries[index];
        const float row_top = list_top + static_cast<float>(index - first) * pitch;
        const SDL_Color color = !entry.enabled      ? ink.disabled
                                : index == selection ? menu_focus_color()
                                                     : ink.row;
        const std::string label = menu_fitted(std::string{entry.label}, reach, cap, MenuFace::display);
        menu_label(label, right, row_top + (pitch - cap) * 0.5F, cap, color, TextAlign::right,
                   MenuFace::display);
        if (entry.enabled)
        {
            // The whole band left of the line, so a row is easy to hit.
            const float width = std::max(menu_width(label, cap, MenuFace::display), 280.0F * unit);
            hits_.push_back({{right - width - 16.0F * unit, row_top, width + 32.0F * unit, pitch},
                             HitKind::menu_item, index});
        }
    }
    if (visible < entries.size())
    {
        // Where the visible rows are in the whole list.
        const float track = pitch * static_cast<float>(visible);
        const float thumb = track * static_cast<float>(visible) / static_cast<float>(entries.size());
        const float x = right + 14.0F * unit;
        fill({x, list_top, 2.0F * unit, track}, {255U, 255U, 255U, 60U});
        fill({x, list_top + (track - thumb) * static_cast<float>(first) /
                               static_cast<float>(entries.size() - visible),
              2.0F * unit, thumb},
             ink.accent);
    }
    // TextRegionElement at (Center.X + 75, 200) in MedLargeFont.
    const ui::MenuEntry &focused = entries[selection];
    Rect words{center + 75.0F * unit, list_top + 25.0F * unit, 0.0F, 0.0F};
    words.width = safe.x + safe.width - words.x - 16.0F * unit;
    words.height = bottom - words.y;
    const float text_cap = menu_cap(menu_cap_text);
    const float used = menu_paragraph(menu_entry_description(focused), words, text_cap, ink.text,
                                      MenuFace::text);
    const float small_cap = menu_cap(menu_cap_medium);
    float y = words.y + used + 26.0F * unit;
    if (!focused.badge.empty() && !focused.enabled)
    {
        menu_label(focused.badge, words.x, y, small_cap, ink.warning, TextAlign::left,
                   MenuFace::text);
        y += menu_line(small_cap);
    }
    if (!note.empty())
        menu_paragraph(note, {words.x, y, words.width, bottom - y}, small_cap, ink.quiet,
                       MenuFace::text);
}

// A menu's rows stand on the left of the picture and their description on the
// right, with the tower between them. A little light is taken out of the two
// sides so white lettering holds against a bright sky; the middle keeps all of
// its own. Nothing is drawn without a picture.
void menu_side_shade()
{
    const float shown = menu_backdrop_shown();
    if (shown <= 0.0F)
        return;
    const float unit = menu_unit();
    const float width = static_cast<float>(metrics_.output_width);
    const float height = static_cast<float>(metrics_.output_height);
    const float center = metrics_.safe.x + metrics_.safe.width * 0.5F;
    const float left_end = center - 70.0F * unit;
    const float right_start = center + 50.0F * unit;
    const float strength = settings_.high_contrast ? 0.55F : 0.36F;
    if (left_end > 0.0F)
    {
        // Full strength under the rows, fading out towards the middle.
        const float fade_start = std::max(0.0F, left_end - 230.0F * unit);
        fill({0.0F, 0.0F, fade_start, height},
             {0U, 0U, 0U, static_cast<Uint8>(std::lround(strength * shown * 255.0F))});
        menu_shade({fade_start, 0.0F, left_end - fade_start, height}, strength * shown, 0.0F, true);
    }
    if (right_start < width)
    {
        const float fade_end = std::min(width, right_start + 230.0F * unit);
        menu_shade({right_start, 0.0F, fade_end - right_start, height}, 0.0F, strength * shown, true);
        fill({fade_end, 0.0F, width - fade_end, height},
             {0U, 0U, 0U, static_cast<Uint8>(std::lround(strength * shown * 255.0F))});
    }
}

// The title screen and the multiplayer hub: the title lettering where the
// originals put their logo, and the rows under it.
void draw_menu_title_screen()
{
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    menu_side_shade();
    draw_menu_title(safe.x + safe.width * 0.5F, safe.y + 44.0F * unit, menu_cap(54.0F));
    const bool multiplayer = menu_.screen() == ui::MenuScreen::multiplayer;
    draw_menu_rows(safe.y + 175.0F * unit,
                   multiplayer ? (steam_identity_verified_ ? std::string_view{"Connected to Steam."}
                                                           : std::string_view{"Steam is not connected."})
                               : std::string_view{});
}

// A plain menu under a heading: game version, game mode, difficulty,
// teleport stations.
void draw_menu_choice_screen(std::string_view heading, std::string_view note = {})
{
    menu_side_shade();
    draw_menu_heading(heading);
    draw_menu_rows(metrics_.safe.y + 175.0F * menu_unit(), note);
}

// ------------------------------------------------------ the in-game menu
//
// InGameMenu: a 50% black wash over the world and the rows centred on the
// screen in the display face, the focused one running to the focus colour.
// TeleportMenu, HostOptions and the player picker are the same screen with
// other rows. OpenMinerZ adds one quiet line under the rows saying what the
// focused row does, and the name of the world at the top.
void draw_in_game_menu()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    const float window_width = static_cast<float>(metrics_.output_width);
    const float window_height = static_cast<float>(metrics_.output_height);
    fill({0.0F, 0.0F, window_width, window_height}, {0U, 0U, 0U, 128U});
    const ui::MenuScreen screen = menu_.screen();
    const auto &entries = menu_.entries();
    const float center = window_width * 0.5F;

    // InGameMenu.Draw: the server message in the top-left corner while online
    // (ConsoleFont, one-pixel outline, 22 units in).
    const float small_cap = menu_cap(menu_cap_small);
    const float medium_cap = menu_cap(menu_cap_medium);
    if (screen == ui::MenuScreen::pause && in_online_session())
    {
        const std::string &server = online_client_ != nullptr ? online_join_server_name_
                                                              : host_settings_.server_name;
        menu_label(menu_fitted("Server Message: " + server, safe.width - 44.0F * unit, small_cap * 1.2F,
                               MenuFace::text),
                   safe.x + 22.0F * unit, safe.y + 8.0F * unit, small_cap * 1.2F, ink.text,
                   TextAlign::left, MenuFace::text);
    }

    // What this is and where: quiet, above the rows.
    const bool frozen =
        !world_simulation_active(WorldOverlay::pause_menu, online_gameplay_active(), false);
    std::string caption = screen == ui::MenuScreen::teleport        ? "Teleport"
                          : screen == ui::MenuScreen::host_options  ? "Host Options"
                          : screen == ui::MenuScreen::select_player ? "Choose a Player"
                          : frozen                                  ? "Game Paused"
                                                                    : "Game Menu";
    std::string where = active_world_path_.empty() ? std::string{game_mode_label(game_mode_)}
                                                   : active_world_path_.stem().string();
    if (screen == ui::MenuScreen::select_player)
    {
        // SelectPlayerScreen's "Players n/max" counts the local player too.
        const std::size_t players = player_pick_gids_.size() + 1U;
        where = online_host_ != nullptr && online_host_->running()
                    ? std::format("Players {}/{}", players, host_settings_.maximum_players)
                    : std::format("Players {}", players);
    }

    float cap = menu_cap(menu_cap_large);
    float pitch = menu_cap(64.0F); // LargeFont's cell: InGameMenu sets no line spacing
    const float header = medium_cap + 14.0F * unit + small_cap + 30.0F * unit;
    const float footer = small_cap * 2.0F + 56.0F * unit;
    const float available =
        std::max(pitch, safe.height - header - footer - 20.0F * unit);
    const std::size_t count = std::max<std::size_t>(entries.size(), 1U);
    std::size_t visible = count;
    std::size_t first = 0U;
    if (pitch * static_cast<float>(count) > available)
    {
        const float fit = std::max(0.60F, available / (pitch * static_cast<float>(count)));
        cap *= fit;
        pitch *= fit;
        visible = std::clamp<std::size_t>(static_cast<std::size_t>(available / pitch), 1U, count);
        if (menu_.selection() >= visible)
            first = std::min(menu_.selection() - visible + 1U, count - visible);
    }
    const float block = header + pitch * static_cast<float>(visible) + footer;
    float y = safe.y + std::max(0.0F, (safe.height - block) * 0.5F);
    menu_label(caption, center, y, medium_cap, ink.heading, TextAlign::center, MenuFace::display);
    y += medium_cap + 14.0F * unit;
    menu_label(menu_fitted(where, safe.width * 0.7F, small_cap, MenuFace::text), center, y, small_cap,
               ink.quiet, TextAlign::center, MenuFace::text);
    y += small_cap + 30.0F * unit;

    const float reach = std::min(safe.width - 40.0F * unit, 900.0F * unit);
    for (std::size_t index = first; index < entries.size() && index < first + visible; ++index)
    {
        const ui::MenuEntry &entry = entries[index];
        const float row_top = y + static_cast<float>(index - first) * pitch;
        const SDL_Color color = !entry.enabled               ? ink.disabled
                                : index == menu_.selection() ? menu_focus_color()
                                                             : ink.row;
        const std::string label = menu_fitted(std::string{entry.label}, reach, cap, MenuFace::display);
        menu_label(label, center, row_top + (pitch - cap) * 0.5F, cap, color, TextAlign::center,
                   MenuFace::display);
        if (entry.enabled)
        {
            const float width = std::max(menu_width(label, cap, MenuFace::display), 320.0F * unit);
            hits_.push_back({{center - width * 0.5F - 16.0F * unit, row_top,
                              width + 32.0F * unit, pitch},
                             HitKind::menu_item, index});
        }
    }
    if (entries.empty())
        menu_label("No other players are in this session.", center, y + (pitch - medium_cap) * 0.5F,
                   medium_cap, ink.quiet, TextAlign::center, MenuFace::text);
    if (visible < entries.size())
    {
        const float track = pitch * static_cast<float>(visible);
        const float thumb = track * static_cast<float>(visible) / static_cast<float>(entries.size());
        const float x = center + reach * 0.5F + 10.0F * unit;
        fill({x, y, 2.0F * unit, track}, {255U, 255U, 255U, 60U});
        fill({x, y + (track - thumb) * static_cast<float>(first) /
                         static_cast<float>(entries.size() - visible),
              2.0F * unit, thumb},
             ink.accent);
    }
    y += pitch * static_cast<float>(visible) + 22.0F * unit;
    if (menu_.selection() < entries.size())
        menu_label(menu_fitted(std::string{entries[menu_.selection()].description},
                               safe.width - 40.0F * unit, small_cap, MenuFace::text),
                   center, y, small_cap, ink.quiet, TextAlign::center, MenuFace::text);
    const bool pad = gamepad_input_active_;
    const std::array<MenuKeyHint, 3> hints{
        {{"Up / Down", "Select"},
         {pad ? "A" : "Enter", "Choose"},
         {pad ? "B" : "Esc", screen == ui::MenuScreen::pause ? "Return to game" : "Back"}}};
    menu_key_hints(hints, center, y + small_cap + 18.0F * unit, safe.width - 40.0F * unit,
                   TextAlign::center);
}

// ----------------------------------------------------------------- casing
//
// Labels and values are Title Case in the originals ("View Distance",
// "Texture Quality", "Regular", "On"). The settings model keeps its own
// wording, so the casing is applied where the words are drawn.
// (openminerz/menu_look.hpp has the rules.)
[[nodiscard]] static std::string menu_title_case(std::string_view value)
{
    return ui::menu_title_case(value);
}

// A value as it is shown: one the model wrote in capitals becomes Title Case;
// anything already in mixed case (a device name, a server name) is left alone.
[[nodiscard]] static std::string menu_value_case(std::string value)
{
    return ui::menu_value_case(std::move(value));
}

// ------------------------------------------------------- shared furniture

// A text button on a black plate, as the originals' Back button is. The
// focused (or pointed-at) one is in the focus colour.
void menu_plate_button(Rect rect, std::string_view label, bool focused, HitKind kind,
                       std::size_t index, float cap_units = menu_cap_medium * 0.86F)
{
    const MenuInk ink = menu_ink();
    // UI area: a mod theme's button image in place of the black plate.
    if (!mod_ui_plate(rect, focused ? modding::ui::ImageKey::button_focus : modding::ui::ImageKey::button))
    {
        fill(rect, {0U, 0U, 0U, focused ? Uint8{215U} : Uint8{165U}});
        if (focused)
            menu_focus_band(rect);
    }
    menu_label_in(label, {rect.x + 8.0F * menu_unit(), rect.y, rect.width - 16.0F * menu_unit(), rect.height},
                  menu_cap(cap_units), focused ? menu_focus_color() : ink.row, TextAlign::center,
                  MenuFace::display);
    hits_.push_back({rect, kind, index});
}

// Where the visible part of a scrolling list is in the whole of it.
void menu_scroll_mark(float x, float top, float height, float visible_share, float position)
{
    const float width = std::max(2.0F, std::round(3.0F * menu_unit()));
    fill({x, top, width, height}, {255U, 255U, 255U, 48U});
    const float thumb = std::max(height * std::clamp(visible_share, 0.05F, 1.0F), 6.0F * menu_unit());
    fill({x, top + (height - thumb) * std::clamp(position, 0.0F, 1.0F), width, thumb},
         menu_ink().accent);
}

// ---------------------------------------------------------------- settings
//
// OpenMinerZ's settings are far more than either original's (seven rows in
// 1.6.3.8, two tabs in 1.9.9.8), so the screen keeps its pages - but a page
// is drawn the way SettingsMenu draws its rows: the label on the left, the
// value between "<" and ">" on the right, bars black with a light fill, the
// focused row in the focus colour. Pages run down the left edge, right-aligned
// like every menu of the originals. Long pages are split into named groups and
// scroll; the focused option is explained underneath in full sentences.

// Where a group of related options begins on a page, and what it is called.
// The pages are flat lists in the settings model (settings_menu.cpp); an
// option nobody has listed here simply belongs to the group above it.
[[nodiscard]] static std::string_view settings_group_at(ui::SettingId id) noexcept
{
    using Id = ui::SettingId;
    switch (id)
    {
    case Id::window_mode:
        return "Screen";
    case Id::exposure_compensation:
        return "Picture";
    case Id::quality_preset:
        return "Preset";
    case Id::presentation:
        return "Look";
    case Id::view_distance:
        return "Detail";
    case Id::bloom:
        return "Light";
    case Id::auto_exposure:
        return "Colour";
    case Id::render_scale:
        return "Frame Rate";
    case Id::low_latency:
        return "Responsiveness";
    case Id::show_fps:
        return "Read-Outs";
    case Id::ui_style:
        return "Style";
    case Id::ui_scale:
        return "Size";
    case Id::hud_server:
        return "Extra Read-Outs";
    case Id::hud_distance:
        return "HUD Elements";
    case Id::subtitles:
        return "Subtitles";
    case Id::audio_output:
        return "Output";
    case Id::master_volume:
        return "Volume";
    case Id::mute_unfocused:
        return "Behaviour";
    default:
        return {};
    }
}

void draw_settings_screen()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    const float top = draw_menu_heading(retail_text(RetailString::Options, menu_generation()));
    const float bottom = safe.y + safe.height - 34.0F * unit;
    const Rect area{safe.x + 8.0F * unit, top - 14.0F * unit, safe.width - 16.0F * unit,
                    bottom - top + 14.0F * unit};
    menu_plate(area, 0.50F);

    // ---- pages -----------------------------------------------------------
    const auto tabs = ui::all_tabs();
    const float small_cap = menu_cap(menu_cap_small);
    const float tab_cap = menu_cap(16.0F);
    float widest_tab = 0.0F;
    for (const ui::SettingsTab tab : tabs)
        widest_tab = std::max(widest_tab, menu_width(menu_title_case(ui::tab_label(tab)), tab_cap,
                                                     MenuFace::display));
    const float column = std::min(widest_tab + 44.0F * unit, area.width * 0.26F);
    const float rule_x = area.x + column;
    const float button_height = 30.0F * unit;
    const float buttons_top = area.y + area.height - 14.0F * unit - button_height * 2.0F - 8.0F * unit;
    const float tabs_top = area.y + 16.0F * unit;
    const float tab_pitch = std::min(
        34.0F * unit, (buttons_top - 10.0F * unit - tabs_top) / static_cast<float>(tabs.size()));
    for (std::size_t index = 0U; index < tabs.size(); ++index)
    {
        const bool current = tabs[index] == menu_.settings_tab();
        const Rect row{area.x + 6.0F * unit, tabs_top + tab_pitch * static_cast<float>(index),
                       column - 12.0F * unit, tab_pitch};
        if (current)
        {
            menu_focus_band(row);
            fill({rule_x - 3.0F * unit, row.y, 3.0F * unit, row.height}, ink.accent);
        }
        menu_label_in(menu_title_case(ui::tab_label(tabs[index])),
                      {row.x, row.y, row.width - 14.0F * unit, row.height},
                      std::min(tab_cap, tab_pitch * 0.56F), current ? ink.focus : ink.row,
                      TextAlign::right, MenuFace::display);
        hits_.push_back({row, HitKind::tab, index});
    }
    fill({rule_x, area.y + 10.0F * unit, std::max(1.0F, std::round(unit)), area.height - 20.0F * unit},
         {255U, 255U, 255U, 70U});
    const bool reset_armed =
        reset_armed_seconds_ > 0.0F && reset_armed_tab_ == menu_.settings_tab();
    const Rect reset{area.x + 12.0F * unit, buttons_top, column - 24.0F * unit, button_height};
    const Rect back{reset.x, reset.y + button_height + 8.0F * unit, reset.width, button_height};
    menu_plate_button(reset, reset_armed ? "Press Again" : "Reset Page", reset_armed,
                      HitKind::reset_page, 0U);
    menu_plate_button(back, "Save & Back", false, HitKind::back, 0U);

    // ---- the page --------------------------------------------------------
    const float content_x = rule_x + 22.0F * unit;
    const float content_width = area.x + area.width - content_x - 22.0F * unit;
    const float title_cap = menu_cap(menu_cap_medium);
    float y = area.y + 16.0F * unit;
    menu_label(menu_title_case(ui::tab_title(menu_.settings_tab())), content_x, y, title_cap,
               ink.heading, TextAlign::left, MenuFace::display);
    y += title_cap + 9.0F * unit;
    menu_label(menu_fitted(std::string{ui::tab_summary(menu_.settings_tab())}, content_width,
                           small_cap, MenuFace::text),
               content_x, y, small_cap, ink.quiet, TextAlign::left, MenuFace::text);
    y += small_cap + 10.0F * unit;
    menu_rule(content_x, y, content_width);
    const float list_top = y + 8.0F * unit;

    const auto &rows = ui::rows_for_tab(menu_.settings_tab());
    if (rows.empty())
        return;
    const std::size_t selection = std::min(menu_.selection(), rows.size() - 1U);

    // The footer: up to three lines about the focused option, then one line
    // of state (why it is inactive, what a restart is waiting for, saved).
    const float note_cap = menu_cap(14.0F);
    const float note_pitch = menu_line(note_cap);
    const float footer_height = note_pitch * 3.0F + small_cap + 30.0F * unit;
    const float footer_top = area.y + area.height - footer_height;
    const float list_bottom = footer_top - 6.0F * unit;

    // Rows and group headings, in drawing order.
    struct Item final
    {
        bool heading{};
        std::size_t row{};
        std::string_view title;
        float height{};
    };
    const float row_height = menu_cap(34.0F);
    const float heading_height = menu_cap(30.0F);
    const bool grouped = rows.size() > 6U;
    std::vector<Item> items;
    std::size_t selected_item = 0U;
    for (std::size_t index = 0U; index < rows.size(); ++index)
    {
        if (grouped)
            if (const std::string_view title = settings_group_at(rows[index].id); !title.empty())
                items.push_back({true, index, title, heading_height});
        if (index == selection)
            selected_item = items.size();
        items.push_back({false, index, {}, row_height});
    }
    // Scroll only as far as it takes to show the focused row (and the heading
    // directly above it), so the list does not jump about.
    if (settings_scroll_tab_ != menu_.settings_tab())
    {
        settings_scroll_tab_ = menu_.settings_tab();
        settings_first_item_ = 0U;
    }
    settings_first_item_ = std::min(settings_first_item_, items.size() - 1U);
    const std::size_t lead =
        selected_item > 0U && items[selected_item - 1U].heading ? selected_item - 1U : selected_item;
    if (lead < settings_first_item_)
        settings_first_item_ = lead;
    const float room = std::max(row_height, list_bottom - list_top);
    const auto height_from = [&items](std::size_t first, std::size_t last) {
        float total = 0.0F;
        for (std::size_t index = first; index <= last && index < items.size(); ++index)
            total += items[index].height;
        return total;
    };
    while (settings_first_item_ < selected_item &&
           height_from(settings_first_item_, selected_item) > room)
        ++settings_first_item_;
    // Do not leave a gap under the last row when everything above could fit.
    while (settings_first_item_ > 0U &&
           height_from(settings_first_item_ - 1U, items.size() - 1U) <= room)
        --settings_first_item_;

    const float label_cap = menu_cap(menu_cap_medium);
    const float value_share = 0.56F; // click_at() splits the value area here
    float row_y = list_top;
    std::size_t last_drawn = settings_first_item_;
    for (std::size_t index = settings_first_item_; index < items.size(); ++index)
    {
        const Item &item = items[index];
        if (row_y + item.height > list_bottom + 1.0F)
            break;
        last_drawn = index;
        if (item.heading)
        {
            const float cap = menu_cap(12.0F);
            const float text_top = row_y + item.height - cap - 7.0F * unit;
            const float width = menu_width(item.title, cap, MenuFace::display);
            menu_label(item.title, content_x + 4.0F * unit, text_top, cap, ink.quiet, TextAlign::left,
                       MenuFace::display);
            menu_rule(content_x + width + 16.0F * unit, text_top + cap * 0.5F,
                      content_width - width - 16.0F * unit, 0.22F);
            row_y += item.height;
            continue;
        }
        const ui::SettingRow &setting = rows[item.row];
        const bool focused = item.row == selection;
        const bool available = ui::setting_unavailable_reason(settings_, setting.id).empty();
        const Rect row{content_x, row_y, content_width, item.height};
        if (focused)
            menu_focus_band(row);
        const SDL_Color label_color = !available ? ink.disabled : focused ? ink.focus : ink.text;
        const SDL_Color quiet_color = !available ? ink.disabled : focused ? ink.focus : ink.quiet;

        // Label, then a tag on an option that only applies after a restart.
        const float label_x = row.x + 12.0F * unit;
        float label_room = row.width * value_share - 24.0F * unit;
        const float tag_cap = menu_cap(10.0F);
        const float tag_width =
            setting.requires_restart ? menu_width("Restart", tag_cap, MenuFace::display) + 14.0F * unit
                                     : 0.0F;
        label_room -= tag_width > 0.0F ? tag_width + 10.0F * unit : 0.0F;
        const float drawn = menu_label_in(menu_title_case(setting.label),
                                          {label_x, row.y, label_room, row.height}, label_cap,
                                          label_color, TextAlign::left, MenuFace::text);
        if (tag_width > 0.0F)
        {
            const Rect tag{label_x + drawn + 10.0F * unit, row.y + (row.height - tag_cap - 10.0F * unit) * 0.5F,
                           tag_width, tag_cap + 10.0F * unit};
            fill(tag, {0U, 0U, 0U, 150U});
            stroke(tag, ink.warning, std::max(1.0F, std::round(unit)));
            menu_label_in("Restart", tag, tag_cap, ink.warning, TextAlign::center,
                          MenuFace::display, false);
        }

        // "<", the value (a bar first, for a level), ">". The arrows stay
        // put whatever the value's length; the midpoint between them is where
        // a click changes from "previous" to "next".
        const float value_x = row.x + row.width * value_share;
        const float value_width = row.width - row.width * value_share;
        const float arrow = 18.0F * unit;
        menu_label_in("<", {value_x + 4.0F * unit, row.y, arrow, row.height}, label_cap, quiet_color,
                      TextAlign::center, MenuFace::text);
        menu_label_in(">", {value_x + value_width - arrow - 4.0F * unit, row.y, arrow, row.height},
                      label_cap, quiet_color, TextAlign::center, MenuFace::text);
        Rect inner{value_x + arrow + 12.0F * unit, row.y, value_width - (arrow + 12.0F * unit) * 2.0F,
                   row.height};
        const std::string value = menu_value_case(setting_display_value(setting.id));
        if (setting.kind == ui::SettingKind::slider)
        {
            // BarSettingItem: a black bar with the level in white inside it.
            // The bar gives way to a long value ("100%  2560 x 1440").
            const float bar_width =
                std::clamp(inner.width - menu_width(value, label_cap, MenuFace::text) - 14.0F * unit,
                           inner.width * 0.22F, inner.width * 0.50F);
            const float bar_height = std::max(6.0F, std::round(10.0F * unit));
            const Rect bar{inner.x, row.y + (row.height - bar_height) * 0.5F, bar_width, bar_height};
            const float edge = std::max(1.0F, std::round(unit));
            fill(bar, {0U, 0U, 0U, 255U});
            fill({bar.x + edge, bar.y + edge,
                  (bar.width - edge * 2.0F) *
                      std::clamp(ui::setting_normalized_value(settings_, setting.id), 0.0F, 1.0F),
                  bar.height - edge * 2.0F},
                 !available ? ink.disabled
                 : menu_generation() == RetailGeneration::modern ? ink.accent
                                                                 : SDL_Color{255U, 255U, 255U, 255U});
            inner.x += bar_width + 12.0F * unit;
            inner.width -= bar_width + 12.0F * unit;
        }
        menu_label_in(value, inner, label_cap, label_color, TextAlign::center, MenuFace::text);
        hits_.push_back({row, HitKind::setting_row, item.row});
        row_y += item.height;
    }
    if (settings_first_item_ > 0U || last_drawn + 1U < items.size())
    {
        const float total = height_from(0U, items.size() - 1U);
        const float before = settings_first_item_ > 0U ? height_from(0U, settings_first_item_ - 1U) : 0.0F;
        const float shown = height_from(settings_first_item_, last_drawn);
        menu_scroll_mark(content_x + content_width + 8.0F * unit, list_top, list_bottom - list_top,
                         shown / std::max(total, 1.0F),
                         total > shown ? before / (total - shown) : 0.0F);
    }

    // ---- what the focused option does --------------------------------------
    menu_rule(content_x, footer_top, content_width);
    menu_paragraph(rows[selection].description,
                   {content_x, footer_top + 12.0F * unit, content_width, note_pitch * 3.0F}, note_cap,
                   ink.text, MenuFace::text);
    const float status_y = footer_top + 12.0F * unit + note_pitch * 3.0F + 2.0F * unit;
    const bool restart_pending = settings_.texture_style != loaded_asset_style_;
    const std::string_view save_state =
        restart_pending ? "Restart needed" : (dirty_ ? "Saving..." : "Saved");
    const float save_width = menu_width(save_state, small_cap, MenuFace::text);
    menu_label(save_state, content_x + content_width, status_y, small_cap,
               restart_pending ? ink.warning : ink.quiet, TextAlign::right, MenuFace::text);
    if (const SettingsFooterNote note = settings_footer_note(rows[selection]); !note.text.empty())
        menu_label(menu_fitted(note.text, content_width - save_width - 20.0F * unit, small_cap,
                               MenuFace::text),
                   content_x, status_y, small_cap, note.attention ? ink.warning : ink.quiet,
                   TextAlign::left, MenuFace::text);

    if (display_prompt_active())
        draw_display_prompt();
}

// A dialog: a black panel in the middle of the screen over a dimmed picture,
// MyriadMedium's family inside, as the originals' PCDialogScreen is. Returns
// the panel; the caller lays out its own body and buttons.
Rect draw_menu_dialog(std::string_view title, float width_units, float height_units)
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect window{0.0F, 0.0F, static_cast<float>(metrics_.output_width),
                      static_cast<float>(metrics_.output_height)};
    fill(window, {0U, 0U, 0U, 150U});
    hits_.push_back({window, HitKind::blocker, 0U});
    const float width = std::min(width_units * unit, metrics_.safe.width - 24.0F * unit);
    const float height = std::min(height_units * unit, metrics_.safe.height - 24.0F * unit);
    const Rect panel{std::round((window.width - width) * 0.5F),
                     std::round((window.height - height) * 0.5F), width, height};
    // UI area: a mod theme's dialog image in place of the framed black panel.
    if (!mod_ui_plate(panel, modding::ui::ImageKey::dialog))
    {
        fill(panel, {0U, 0U, 0U, 228U});
        const float edge = std::max(2.0F, std::round(2.0F * unit));
        stroke(panel, {255U, 255U, 255U, 200U}, edge);
    }
    const float cap = menu_cap(menu_cap_text);
    menu_label_in(title, {panel.x + 24.0F * unit, panel.y + 16.0F * unit, panel.width - 48.0F * unit, cap},
                  cap, ink.heading, TextAlign::left, MenuFace::display);
    menu_rule(panel.x + 24.0F * unit, panel.y + 16.0F * unit + cap + 14.0F * unit,
              panel.width - 48.0F * unit, 0.4F);
    return panel;
}

// A screen that is a table or a list rather than a menu: its heading, and
// the plate its contents stand on (between the heading and the key hints).
Rect draw_menu_table_screen(std::string_view heading)
{
    const float unit = menu_unit();
    const Rect safe = metrics_.safe;
    const float top = draw_menu_heading(heading);
    const float bottom = safe.y + safe.height - 34.0F * unit;
    const Rect area{safe.x + 8.0F * unit, top - 14.0F * unit, safe.width - 16.0F * unit,
                    bottom - top + 14.0F * unit};
    menu_plate(area, 0.50F);
    return area;
}

// A row of a list drawn as the originals' menu rows are: no box, the focused
// one in the focus colour on a faint band.
void menu_text_row(Rect row, std::string_view label, std::size_t index, float cap_units,
                   TextAlign align = TextAlign::left)
{
    const MenuInk ink = menu_ink();
    const bool focused = menu_.selection() == index;
    const bool enabled = index >= menu_.entries().size() || menu_.entries()[index].enabled;
    if (focused)
        menu_focus_band(row);
    menu_label_in(label, {row.x + 10.0F * menu_unit(), row.y, row.width - 20.0F * menu_unit(), row.height},
                  menu_cap(cap_units),
                  !enabled ? ink.disabled : focused ? menu_focus_color() : ink.row, align,
                  MenuFace::display);
    if (enabled)
        hits_.push_back({row, HitKind::menu_item, index});
}

// ----------------------------------------------------------- world library
//
// ChooseSavedWorldScreen is a list: "New World" first, then each save in
// MedFont with its dates in SmallFont. OpenMinerZ's library keeps that list
// on the left and adds what the original has no room for on the right: the
// picture taken when the world was last left, its history and its rules.
void draw_world_library()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const auto &entries = menu_.entries();
    if (entries.size() < 2U)
        return;
    const RetailGeneration generation = menu_generation();
    const Rect area = draw_menu_table_screen(retail_text(RetailString::Choose_a_World, generation));
    const std::size_t back_index = entries.size() - 1U;
    if (menu_.selection() > 0U && menu_.selection() <= world_slots_.size())
        world_detail_index_ = menu_.selection() - 1U;
    if (!world_slots_.empty())
        world_detail_index_ = std::min(world_detail_index_, world_slots_.size() - 1U);
    WorldSlot *selected = world_slots_.empty() ? nullptr : &world_slots_[world_detail_index_];

    const float pad = 14.0F * unit;
    const float small_cap = menu_cap(menu_cap_small);
    const float name_cap = menu_cap(16.0F);
    const float left_width = std::min(area.width * 0.36F, 440.0F * unit);
    const Rect left{area.x + pad, area.y + pad, left_width, area.height - pad * 2.0F};
    const Rect detail{left.x + left.width + 18.0F * unit, left.y,
                      area.x + area.width - pad - (left.x + left.width + 18.0F * unit), left.height};

    // ---- the list ----------------------------------------------------------
    const float row_height = menu_cap(40.0F);
    menu_text_row({left.x, left.y, left.width, row_height},
                  retail_text(RetailString::New_World, generation), 0U, menu_cap_medium);
    const Rect sort{left.x, left.y + row_height + 4.0F * unit, left.width, small_cap + 14.0F * unit};
    constexpr std::array<std::string_view, 3> sort_labels{"Last Played", "World Name", "Playtime"};
    menu_label_in(std::format("{} saved world{}", world_slots_.size(),
                              world_slots_.size() == 1U ? "" : "s"),
                  {sort.x + 10.0F * unit, sort.y, sort.width * 0.5F, sort.height}, small_cap, ink.quiet,
                  TextAlign::left, MenuFace::text);
    menu_label_in(std::format("Sort: {}  [F]", sort_labels[static_cast<std::size_t>(world_sort_) % 3U]),
                  {sort.x + sort.width * 0.5F, sort.y, sort.width * 0.5F - 10.0F * unit, sort.height},
                  small_cap, ink.quiet, TextAlign::right, MenuFace::text);
    hits_.push_back({{sort.x + sort.width * 0.5F, sort.y, sort.width * 0.5F, sort.height},
                     HitKind::world_sort, 0U});
    menu_rule(left.x, sort.y + sort.height, left.width, 0.25F);

    const Rect back{left.x, left.y + left.height - row_height, left.width, row_height};
    menu_text_row(back, retail_text(RetailString::Back, generation), back_index, menu_cap_medium);
    const float list_top = sort.y + sort.height + 6.0F * unit;
    const float list_bottom = back.y - 6.0F * unit;
    const float slot_height = menu_cap(64.0F);
    const std::size_t visible = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::max(0.0F, list_bottom - list_top) / slot_height), 1U, 12U);
    const std::size_t first =
        world_slots_.size() <= visible || world_detail_index_ < visible
            ? 0U
            : std::min(world_detail_index_ - visible + 1U, world_slots_.size() - visible);
    const std::size_t last = std::min(world_slots_.size(), first + visible);
    for (std::size_t index = 0U; index < world_slots_.size(); ++index)
        if (index != world_detail_index_ && (index < first || index >= last))
        {
            // Pictures are only kept for the rows on screen.
            world_slots_[index].preview.reset();
            world_slots_[index].preview_checked = false;
        }
    for (std::size_t index = first; index < last; ++index)
    {
        WorldSlot &slot = world_slots_[index];
        const bool shown = index == world_detail_index_;
        const bool focused = menu_.selection() == index + 1U;
        const Rect row{left.x, list_top + static_cast<float>(index - first) * slot_height, left.width,
                       slot_height - 4.0F * unit};
        if (shown)
            menu_focus_band(row);
        const float thumb_height = row.height - 10.0F * unit;
        const Rect thumb{row.x + 5.0F * unit, row.y + 5.0F * unit, thumb_height * 16.0F / 9.0F,
                         thumb_height};
        draw_world_picture(&slot, thumb);
        const float text_x = thumb.x + thumb.width + 12.0F * unit;
        const float text_width = row.x + row.width - text_x - 8.0F * unit;
        const float line = (row.height - name_cap - small_cap * 2.0F) / 4.0F;
        menu_label(menu_fitted(slot.name, text_width, name_cap, MenuFace::display), text_x,
                   row.y + line, name_cap, focused ? menu_focus_color() : shown ? ink.focus : ink.row,
                   TextAlign::left, MenuFace::display);
        menu_label(menu_fitted(slot.readable ? menu_value_case(slot.mode) + "  -  " +
                                                   menu_value_case(slot.content_style)
                                             : std::string{"Save unavailable"},
                               text_width, small_cap, MenuFace::text),
                   text_x, row.y + line * 2.0F + name_cap, small_cap,
                   slot.readable ? ink.text : ink.warning, TextAlign::left, MenuFace::text);
        menu_label(menu_fitted(slot.saved_at, text_width, small_cap, MenuFace::text), text_x,
                   row.y + line * 3.0F + name_cap + small_cap, small_cap, ink.quiet, TextAlign::left,
                   MenuFace::text);
        hits_.push_back({row, HitKind::world_item, index + 1U});
    }
    if (world_slots_.empty())
    {
        // ChooseSavedWorldScreen's own words for an empty list.
        menu_label(retail_text(RetailString::No_Worlds_Found, generation), left.x + 10.0F * unit,
                   list_top + 14.0F * unit, name_cap, ink.text, TextAlign::left, MenuFace::display);
        menu_label("Choose New World to begin.", left.x + 10.0F * unit,
                   list_top + 14.0F * unit + name_cap + 12.0F * unit, small_cap, ink.quiet,
                   TextAlign::left, MenuFace::text);
    }
    else if (world_slots_.size() > visible)
        menu_scroll_mark(left.x + left.width + 4.0F * unit, list_top,
                         slot_height * static_cast<float>(visible),
                         static_cast<float>(visible) / static_cast<float>(world_slots_.size()),
                         static_cast<float>(first) /
                             static_cast<float>(world_slots_.size() - visible));

    // ---- the selected world --------------------------------------------------
    const float text_cap = menu_cap(14.0F);
    const float value_cap = menu_cap(menu_cap_medium);
    const float button_height = menu_cap(40.0F);
    const float hero_height =
        std::clamp(detail.height * 0.42F, 70.0F * unit, detail.width * 9.0F / 16.0F);
    const Rect hero{detail.x, detail.y, detail.width, hero_height};
    draw_world_picture(selected, hero);
    menu_shade({hero.x, hero.y + hero.height * 0.45F, hero.width, hero.height * 0.55F}, 0.0F, 0.85F);
    const float title_cap = menu_cap(26.0F);
    menu_label(menu_fitted(selected != nullptr ? selected->name : std::string{"New World"},
                           hero.width - 28.0F * unit, title_cap, MenuFace::display),
               hero.x + 14.0F * unit, hero.y + hero.height - title_cap - small_cap - 22.0F * unit,
               title_cap, ink.row, TextAlign::left, MenuFace::display);
    if (selected != nullptr)
        menu_label(menu_fitted((selected->recovered  ? "Recovered backup  -  "
                                : selected->readable ? "Last played  -  "
                                                     : "File updated  -  ") +
                                   selected->saved_at,
                               hero.width - 28.0F * unit, small_cap, MenuFace::text),
                   hero.x + 14.0F * unit, hero.y + hero.height - small_cap - 10.0F * unit, small_cap,
                   ink.quiet, TextAlign::left, MenuFace::text);
    if (selected != nullptr && !selected->preview)
        menu_label(selected->readable ? "A picture is saved when you leave this world."
                                      : "No picture.",
                   hero.x + 14.0F * unit, hero.y + 12.0F * unit, small_cap, ink.quiet,
                   TextAlign::left, MenuFace::text);

    float y = hero.y + hero.height + 14.0F * unit;
    const float action_top = detail.y + detail.height - button_height;
    if (selected != nullptr && selected->readable)
    {
        // Four figures, then the facts in two columns for as many rows as fit.
        const std::array<std::string, 4> figures{
            selected->history.tracked_since_unix_seconds > 0
                ? world_playtime(selected->history.play_seconds)
                : std::string{"Not recorded"},
            std::format("Day {}", static_cast<int>(std::floor(selected->day)) + 1),
            std::format("{:L}", selected->modified_blocks),
            std::format("{:L} m", selected->max_distance)};
        constexpr std::array<std::string_view, 4> figure_labels{"Playtime", "World day",
                                                                "Blocks edited", "Distance"};
        const float cell = detail.width / 4.0F;
        for (std::size_t index = 0U; index < figures.size(); ++index)
        {
            const float x = detail.x + cell * static_cast<float>(index);
            menu_label(menu_fitted(figures[index], cell - 10.0F * unit, value_cap, MenuFace::text), x,
                       y, value_cap, ink.text, TextAlign::left, MenuFace::text);
            menu_label(figure_labels[index], x, y + value_cap + 8.0F * unit, small_cap, ink.quiet,
                       TextAlign::left, MenuFace::text);
        }
        y += value_cap + small_cap + 20.0F * unit;
        menu_rule(detail.x, y, detail.width, 0.25F);
        y += 10.0F * unit;
        const std::string created = selected->history.created_unix_seconds > 0
                                        ? world_saved_at(selected->history.created_unix_seconds, {})
                                        : std::string{"Not recorded"};
        const std::array<std::pair<std::string_view, std::string>, 8> facts{{
            {"Created by", selected->owner},
            {"Created", created},
            {"Game rules",
             menu_value_case(selected->mode) + " / " + menu_value_case(selected->difficulty)},
            {"Last position", selected->position},
            {"World seed", std::to_string(selected->seed)},
            {"World storage",
             selected->file_bytes < 1024U * 1024U
                 ? std::format("{} crates / {:.1f} KB", selected->crates,
                               static_cast<double>(selected->file_bytes) / 1024.0)
                 : std::format("{} crates / {:.1f} MB", selected->crates,
                               static_cast<double>(selected->file_bytes) / (1024.0 * 1024.0))},
            {"Player health",
             selected->player_recorded
                 ? std::format("{:.0f} / 100", std::clamp(selected->health, 0.0F, 100.0F))
                 : std::string{"Not recorded"}},
            {"Game version", menu_value_case(selected->content_style)},
        }};
        const float fact_pitch = text_cap + 13.0F * unit;
        const std::size_t fact_rows = std::min<std::size_t>(
            4U, static_cast<std::size_t>(std::max(0.0F, action_top - 8.0F * unit - y) / fact_pitch));
        const float column = detail.width * 0.5F;
        const float label_width = std::min(column * 0.40F, 130.0F * unit);
        for (std::size_t index = 0U; index < fact_rows * 2U; ++index)
        {
            const float x = detail.x + (index % 2U != 0U ? column + 8.0F * unit : 0.0F);
            const float fact_y = y + static_cast<float>(index / 2U) * fact_pitch;
            menu_label(facts[index].first, x, fact_y + (text_cap - small_cap), small_cap, ink.quiet,
                       TextAlign::left, MenuFace::text);
            menu_label(menu_fitted(facts[index].second, column - label_width - 16.0F * unit, text_cap,
                                   MenuFace::text),
                       x + label_width, fact_y, text_cap, ink.text, TextAlign::left, MenuFace::text);
        }
        const WorldHistory &history = selected->history;
        menu_label_in(history.tracked_since_unix_seconds > 0
                          ? std::format("{} sessions  -  {} deaths  -  tracked since {}",
                                        history.sessions, history.deaths,
                                        world_saved_at(history.tracked_since_unix_seconds, {})
                                            .substr(0, 10))
                          : std::string{"No play history yet."},
                      {detail.x, action_top, detail.width - 230.0F * unit, button_height}, small_cap,
                      ink.quiet, TextAlign::left, MenuFace::text);
        menu_plate_button({detail.x + detail.width - 220.0F * unit, action_top, 220.0F * unit,
                           button_height},
                          hosting_flow_ ? "Host World" : "Continue",
                          menu_.selection() == world_detail_index_ + 1U, HitKind::menu_item,
                          world_detail_index_ + 1U, menu_cap_medium);
    }
    else
    {
        menu_label(selected != nullptr ? "This save could not be opened."
                                       : "Create a world to start playing.",
                   detail.x, y, value_cap, ink.text, TextAlign::left, MenuFace::text);
        menu_paragraph(selected != nullptr
                           ? "The world and its backup are unavailable. Your files have been kept."
                           : "New World starts a fresh world with the game version, mode and "
                             "difficulty you chose.",
                       {detail.x, y + value_cap + 14.0F * unit, detail.width, action_top - y},
                       text_cap, ink.quiet, MenuFace::text);
        if (selected == nullptr)
            menu_plate_button({detail.x + detail.width - 220.0F * unit, action_top, 220.0F * unit,
                               button_height},
                              retail_text(RetailString::New_World, generation),
                              menu_.selection() == 0U, HitKind::menu_item, 0U, menu_cap_medium);
    }
}

// ---------------------------------------------------------- server browser
//
// ChooseOnlineGameScreen: "Choose A Server" over a list in MedFont. Ours has
// columns (which game version a server speaks matters here) and its actions
// along the top.
void draw_server_list()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const auto &entries = menu_.entries();
    const std::size_t count = join_row_sessions_.size();
    if (entries.size() < count + 4U)
        return;
    const RetailGeneration generation = menu_generation();
    const Rect area = draw_menu_table_screen(retail_text(RetailString::Choose_A_Server, generation));
    const float pad = 14.0F * unit;
    const Rect body{area.x + pad, area.y + pad, area.width - pad * 2.0F, area.height - pad * 2.0F};
    const float small_cap = menu_cap(menu_cap_small);
    const float cell_cap = menu_cap(14.0F);
    const float name_cap = menu_cap(16.0F);

    const float gap = 8.0F * unit;
    const float button_width = (body.width - gap * 3.0F) * 0.25F;
    const float button_height = menu_cap(34.0F);
    const std::array<std::string, 4> labels{
        std::string{retail_text(RetailString::Search_Again, generation)},
        "Show: " + menu_value_case(std::string{join_filter_label(join_browser_.options().filter)}),
        "Direct Connect", std::string{retail_text(RetailString::Back, generation)}};
    for (std::size_t index = 0U; index < labels.size(); ++index)
        menu_plate_button({body.x + static_cast<float>(index) * (button_width + gap), body.y,
                           button_width, button_height},
                          labels[index], menu_.selection() == count + index, HitKind::menu_item,
                          count + index);
    float y = body.y + button_height + 12.0F * unit;
    menu_label(menu_fitted(join_browser_status_, body.width - 160.0F * unit, small_cap, MenuFace::text),
               body.x, y, small_cap, ink.quiet, TextAlign::left, MenuFace::text);
    menu_label(std::format("{} server{}", count, count == 1U ? "" : "s"), body.x + body.width, y,
               small_cap, ink.quiet, TextAlign::right, MenuFace::text);
    y += small_cap + 9.0F * unit;
    // Why the last join ended, kept until the next attempt.
    if (!join_failure_text_.empty())
    {
        menu_label(menu_fitted(join_failure_text_, body.width, small_cap, MenuFace::text), body.x, y,
                   small_cap, ink.warning, TextAlign::left, MenuFace::text);
        y += small_cap + 9.0F * unit;
    }
    constexpr std::array<float, 6> columns{0.015F, 0.47F, 0.63F, 0.75F, 0.86F, 0.99F};
    constexpr std::array<std::string_view, 5> headings{"Server", "Version", "Players", "Ping",
                                                       "Access"};
    for (std::size_t column = 0U; column < headings.size(); ++column)
        menu_label(headings[column], body.x + body.width * columns[column], y, small_cap, ink.quiet,
                   TextAlign::left, MenuFace::display);
    y += small_cap + 8.0F * unit;
    menu_rule(body.x, y, body.width, 0.3F);
    const float list_top = y + 6.0F * unit;
    const float bottom = body.y + body.height;
    const float row_height = menu_cap(50.0F);
    const std::size_t visible = std::max<std::size_t>(
        1U, static_cast<std::size_t>(std::max(row_height, bottom - list_top) / row_height));
    const std::size_t first =
        count > visible && menu_.selection() < count
            ? std::min(menu_.selection() >= visible ? menu_.selection() - visible + 1U : 0U,
                       count - visible)
            : 0U;
    for (std::size_t index = first; index < count && index < first + visible; ++index)
    {
        const auto session_index = join_row_sessions_[index];
        const auto &session = join_browser_.sessions()[session_index];
        const auto assessment = browser_join_assessment(session_index);
        const bool focused = menu_.selection() == index;
        const Rect row{body.x, list_top + static_cast<float>(index - first) * row_height, body.width,
                       row_height - 4.0F * unit};
        if (focused)
            menu_focus_band(row);
        const float name_x = row.x + row.width * columns[0];
        const float name_width = row.width * (columns[1] - columns[0]) - 16.0F * unit;
        const float line = (row.height - name_cap - small_cap) / 3.0F;
        menu_label(menu_fitted(session.server_name, name_width, name_cap, MenuFace::display), name_x,
                   row.y + line, name_cap, focused ? menu_focus_color() : ink.row, TextAlign::left,
                   MenuFace::display);
        menu_label(menu_fitted(session.host_name, name_width, small_cap, MenuFace::text), name_x,
                   row.y + line * 2.0F + name_cap, small_cap, ink.quiet, TextAlign::left,
                   MenuFace::text);
        const std::array<std::string, 4> cells{
            std::string{join_protocol_label(session, assessment)},
            std::format("{} / {}", session.current_players, session.maximum_players),
            session.ping_milliseconds.has_value() ? std::format("{} ms", *session.ping_milliseconds)
                                                  : std::string{},
            std::string{session.current_players >= session.maximum_players ? "Full"
                        : !assessment.joinable()                           ? "Unavailable"
                        : session.password_required                        ? "Password"
                        // (Authority) the host decides health, damage and items
                        : session.required_manifest.has_value() &&
                                session.required_manifest->openminerz_only ? "OpenMinerZ"
                                                                           : "Open"}};
        for (std::size_t column = 1U; column < 5U; ++column)
            menu_label_in(cells[column - 1U],
                          {row.x + row.width * columns[column], row.y,
                           row.width * (columns[column + 1U] - columns[column]) - 8.0F * unit,
                           row.height},
                          cell_cap, column == 1U ? protocol_color(assessment.protocol) : ink.text,
                          TextAlign::left, MenuFace::text);
        hits_.push_back({row, HitKind::menu_item, index});
    }
    if (count == 0U)
    {
        // Searching, unreachable and empty are different answers; the list
        // keeps refreshing by itself in all three.
        const bool searching = join_browser_state_ == JoinBrowserState::searching;
        const bool failed = join_browser_state_ == JoinBrowserState::failed;
        const float middle = list_top + (bottom - list_top) * 0.36F;
        menu_label(searching ? std::string_view{"Searching for Games..."}
                   : failed  ? std::string_view{"Could Not Reach Steam"}
                             : retail_text(RetailString::No_Servers_Available, generation),
                   body.x + body.width * 0.5F, middle, menu_cap(menu_cap_text), ink.row,
                   TextAlign::center, MenuFace::display);
        menu_label(searching ? "Asking Steam for Classic and Modern servers."
                   : failed  ? "Trying again automatically. Check that Steam is running."
                             : "The list refreshes automatically. Try showing another kind.",
                   body.x + body.width * 0.5F, middle + menu_cap(menu_cap_text) + 16.0F * unit,
                   cell_cap, ink.quiet, TextAlign::center, MenuFace::text);
    }
    if (count > visible)
        menu_scroll_mark(body.x + body.width + 4.0F * unit, list_top,
                         row_height * static_cast<float>(visible),
                         static_cast<float>(visible) / static_cast<float>(count),
                         static_cast<float>(first) / static_cast<float>(count - visible));
}

// One server, before joining it.
void draw_server_details()
{
    const auto *session = selected_join_session();
    if (session == nullptr)
        return;
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const auto assessment = assess_join_session(*session);
    const Rect area = draw_menu_table_screen("Server Details");
    const float width = std::min(area.width - 28.0F * unit, 880.0F * unit);
    const Rect body{area.x + (area.width - width) * 0.5F, area.y + 18.0F * unit, width,
                    area.height - 36.0F * unit};
    const float text_cap = menu_cap(14.0F);
    const float medium_cap = menu_cap(menu_cap_medium);
    const float title_cap = menu_cap(menu_cap_text);
    float y = body.y;
    const std::string_view protocol = net::protocol_label(assessment.protocol);
    const float protocol_width = menu_width(protocol, medium_cap, MenuFace::display);
    menu_label(menu_fitted(session->server_name, body.width - protocol_width - 20.0F * unit, title_cap,
                           MenuFace::display),
               body.x, y, title_cap, ink.row, TextAlign::left, MenuFace::display);
    menu_label(protocol, body.x + body.width, y + (title_cap - medium_cap), medium_cap,
               protocol_color(assessment.protocol), TextAlign::right, MenuFace::display);
    y += title_cap + 12.0F * unit;
    menu_label(menu_fitted(std::format("{} {}",
                                       retail_text(RetailString::Hosted_By, menu_generation()),
                                       session->host_name),
                           body.width, text_cap, MenuFace::text),
               body.x, y, text_cap, ink.quiet, TextAlign::left, MenuFace::text);
    y += text_cap + 14.0F * unit;
    menu_rule(body.x, y, body.width, 0.3F);
    y += 16.0F * unit;
    menu_label(std::format("{} / {} players", session->current_players, session->maximum_players),
               body.x, y, medium_cap, ink.text, TextAlign::left, MenuFace::text);
    menu_label(session->password_required ? "Password required" : "Open to join",
               body.x + body.width * 0.5F, y, medium_cap, ink.text, TextAlign::left, MenuFace::text);
    y += medium_cap + 18.0F * unit;
    // (Authority) Known before joining: an OpenMinerZ-only server (its own
    // choice or its server mods) decides health, damage and items itself.
    if (session->required_manifest.has_value() && net::requires_extension(*session->required_manifest))
    {
        menu_label(menu_fitted(session->required_manifest->openminerz_only
                                   ? "OpenMinerZ players only  -  the host decides health, damage and items"
                                   : "Server mods  -  the host decides health, damage and items",
                               body.width, text_cap, MenuFace::text),
                   body.x, y, text_cap, ink.quiet, TextAlign::left, MenuFace::text);
        y += text_cap + 14.0F * unit;
    }
    const float button_height = menu_cap(40.0F);
    const float button_y = body.y + body.height - button_height;
    const float notice_height = menu_line(text_cap) * 3.0F;
    y += menu_paragraph(session->server_message,
                        {body.x, y, body.width, std::max(text_cap, button_y - notice_height - y - 20.0F * unit)},
                        text_cap, ink.text, MenuFace::text) +
         16.0F * unit;
    if (!assessment.joinable())
        menu_paragraph(net::join_compatibility_label(assessment.status),
                       {body.x, y, body.width, notice_height}, text_cap, ink.warning, MenuFace::text);
    // The reason the last attempt on this server ended stays readable here
    // after its notification has faded.
    else if (!join_failure_text_.empty() && join_failure_lobby_ == session->session_id)
        menu_paragraph(join_failure_text_, {body.x, y, body.width, notice_height}, text_cap,
                       ink.warning, MenuFace::text);
    const float gap = 10.0F * unit;
    const float button_width = (body.width - gap * 2.0F) / 3.0F;
    const std::array<std::string, 3> labels{
        "Join Game", join_password_.empty() ? std::string{"Password"} : std::string{"Password: Set"},
        std::string{retail_text(RetailString::Back, menu_generation())}};
    for (std::size_t index = 0U; index < labels.size(); ++index)
        menu_plate_button({body.x + static_cast<float>(index) * (button_width + gap), button_y,
                           button_width, button_height},
                          labels[index], menu_.selection() == index, HitKind::menu_item, index,
                          menu_cap_medium);
}

// ------------------------------------------------------ host server settings
//
// Server details, access and game rules: rows with a value, drawn as the
// originals' settings rows are.
void draw_host_rows()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const auto &entries = menu_.entries();
    const ui::MenuScreen screen = menu_.screen();
    const Rect area =
        draw_menu_table_screen(screen == ui::MenuScreen::host_details  ? "Server Details"
                               : screen == ui::MenuScreen::host_access ? "Access & Privacy"
                                                                       : "Game Rules");
    if (entries.empty())
        return;
    const float pad = 16.0F * unit;
    const float width = std::min(area.width - pad * 2.0F, 900.0F * unit);
    const Rect body{area.x + (area.width - width) * 0.5F, area.y + pad, width, area.height - pad * 2.0F};
    const float label_cap = menu_cap(menu_cap_medium);
    const float text_cap = menu_cap(14.0F);
    const float row_height = menu_cap(40.0F);
    const std::size_t selection = std::min(menu_.selection(), entries.size() - 1U);
    for (std::size_t index = 0U; index < entries.size(); ++index)
    {
        const ui::MenuEntry &entry = entries[index];
        const bool focused = index == selection;
        const Rect row{body.x, body.y + static_cast<float>(index) * row_height, body.width,
                       row_height - 2.0F * unit};
        if (focused)
            menu_focus_band(row);
        const SDL_Color color = focused ? ink.focus : ink.text;
        const std::string value = host_setting_value(entry.command);
        menu_label_in(entry.label, {row.x + 12.0F * unit, row.y, row.width * 0.5F - 20.0F * unit, row.height},
                      label_cap, color, TextAlign::left, MenuFace::text);
        if (!value.empty())
        {
            const float value_x = row.x + row.width * 0.5F;
            const float value_width = row.width * 0.5F;
            const float arrow = 18.0F * unit;
            menu_label_in("<", {value_x + 4.0F * unit, row.y, arrow, row.height}, label_cap,
                          focused ? ink.focus : ink.quiet, TextAlign::center, MenuFace::text);
            menu_label_in(">", {value_x + value_width - arrow - 4.0F * unit, row.y, arrow, row.height},
                          label_cap, focused ? ink.focus : ink.quiet, TextAlign::center, MenuFace::text);
            menu_label_in(menu_value_case(value),
                          {value_x + arrow + 12.0F * unit, row.y, value_width - (arrow + 12.0F * unit) * 2.0F,
                           row.height},
                          label_cap,
                          entry.command == ui::MenuCommand::select_host_protocol && !focused
                              ? protocol_color(host_settings_.protocol)
                              : color,
                          TextAlign::center, MenuFace::text);
        }
        hits_.push_back({row, HitKind::menu_item, index});
    }
    float y = body.y + static_cast<float>(entries.size()) * row_height + 10.0F * unit;
    menu_rule(body.x, y, body.width, 0.3F);
    y += 14.0F * unit;
    y += menu_paragraph(entries[selection].description, {body.x, y, body.width, menu_line(text_cap) * 3.0F},
                        text_cap, ink.text, MenuFace::text) +
         20.0F * unit;
    // What players will see in the server list.
    if (y + label_cap * 2.0F + 20.0F * unit < body.y + body.height)
    {
        menu_label(menu_fitted(host_settings_.server_name, body.width * 0.7F, label_cap, MenuFace::display),
                   body.x, y, label_cap, ink.row, TextAlign::left, MenuFace::display);
        menu_label(net::protocol_label(host_settings_.protocol), body.x + body.width, y, label_cap,
                   protocol_color(host_settings_.protocol), TextAlign::right, MenuFace::display);
        y += label_cap + 12.0F * unit;
        menu_label(menu_fitted(std::format("Up to {} players  -  {}  -  {}  -  PVP: {}{}",
                                           host_settings_.maximum_players,
                                           menu_value_case(std::string{
                                               net::visibility_label(host_settings_.visibility)}),
                                           host_settings_.password.empty() ? "no password"
                                                                           : "password required",
                                           menu_value_case(std::string{
                                               net::pvp_label(host_settings_.player_versus_player)}),
                                           // (Authority) the lobby says so too
                                           net::requires_extension(host_settings_.mod_manifest)
                                               ? "  -  OpenMinerZ only, the host decides"
                                               : ""),
                               body.width, text_cap, MenuFace::text),
                   body.x, y, text_cap, ink.quiet, TextAlign::left, MenuFace::text);
    }
}

// ------------------------------------------------------------------ awards
//
// The originals' Awards screen lists each award's name and how it is earned;
// ours adds how far along each one is.
void draw_awards()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect area = draw_menu_table_screen(retail_text(RetailString::Awards, menu_generation()));
    const auto definitions = achievement_definitions();
    if (definitions.empty())
        return;
    const std::size_t selected = std::min(menu_.selection(), definitions.size() - 1U);
    const AchievementDefinition &award = definitions[selected];
    const bool unlocked = progress_.unlocked_at[selected] > 0;
    const SDL_Color gold{255U, 189U, 0U, 255U}; // CMZColors.Gold
    const float pad = 14.0F * unit;
    const float small_cap = menu_cap(menu_cap_small);
    const float text_cap = menu_cap(14.0F);
    const float button_height = menu_cap(34.0F);
    const Rect body{area.x + pad, area.y + pad, area.width - pad * 2.0F,
                    area.height - pad * 2.0F - button_height - 8.0F * unit};
    const float list_width = body.width * 0.50F;
    const float pitch = menu_cap(40.0F);
    const std::size_t visible =
        std::max<std::size_t>(1U, static_cast<std::size_t>(body.height / pitch));
    if (selected < achievement_first_visible_)
        achievement_first_visible_ = selected;
    if (selected >= achievement_first_visible_ + visible)
        achievement_first_visible_ = selected + 1U - visible;
    achievement_first_visible_ = std::min(
        achievement_first_visible_, definitions.size() > visible ? definitions.size() - visible : 0U);
    const std::size_t first = achievement_first_visible_;
    for (std::size_t index = first; index < std::min(definitions.size(), first + visible); ++index)
    {
        const AchievementDefinition &entry = definitions[index];
        const bool done = progress_.unlocked_at[index] > 0;
        const float fraction =
            done ? 1.0F
                 : static_cast<float>(
                       std::clamp(progress_.value(entry.metric) / entry.target, 0.0, 1.0));
        const Rect row{body.x, body.y + static_cast<float>(index - first) * pitch, list_width,
                       pitch - 3.0F * unit};
        const bool focused = index == selected;
        if (focused)
            menu_focus_band(row);
        const float mark = 64.0F * unit;
        menu_label_in(entry.name, {row.x + 10.0F * unit, row.y, row.width - mark - 20.0F * unit, row.height - 5.0F * unit},
                      menu_cap(15.0F), focused ? menu_focus_color() : done ? gold : ink.row,
                      TextAlign::left, MenuFace::display);
        menu_label_in(done ? std::string{"Done"} : std::format("{}%", static_cast<int>(fraction * 100.0F)),
                      {row.x + row.width - mark - 8.0F * unit, row.y, mark, row.height - 5.0F * unit},
                      small_cap, done ? gold : ink.quiet, TextAlign::right, MenuFace::text);
        const Rect bar{row.x + 10.0F * unit, row.y + row.height - 4.0F * unit, row.width - 20.0F * unit,
                       std::max(2.0F, std::round(2.0F * unit))};
        fill(bar, {0U, 0U, 0U, 200U});
        fill({bar.x, bar.y, bar.width * fraction, bar.height}, done ? gold : ink.accent);
        hits_.push_back({row, HitKind::menu_item, index});
    }
    if (definitions.size() > visible)
        menu_scroll_mark(body.x + list_width + 4.0F * unit, body.y, pitch * static_cast<float>(visible),
                         static_cast<float>(visible) / static_cast<float>(definitions.size()),
                         static_cast<float>(first) / static_cast<float>(definitions.size() - visible));

    const Rect detail{body.x + list_width + 24.0F * unit, body.y,
                      body.width - list_width - 24.0F * unit, body.height};
    float y = detail.y + 4.0F * unit;
    menu_label(unlocked ? "Unlocked" : "In progress", detail.x, y, small_cap,
               unlocked ? gold : ink.quiet, TextAlign::left, MenuFace::display);
    y += small_cap + 14.0F * unit;
    const float name_cap = menu_cap(menu_cap_text);
    menu_label(menu_fitted(std::string{award.name}, detail.width, name_cap, MenuFace::display), detail.x,
               y, name_cap, ink.row, TextAlign::left, MenuFace::display);
    y += name_cap + 16.0F * unit;
    y += menu_paragraph(award.description, {detail.x, y, detail.width, menu_line(text_cap) * 4.0F},
                        text_cap, ink.text, MenuFace::text) +
         16.0F * unit;
    if (unlocked)
        menu_label(world_saved_at(progress_.unlocked_at[selected], {}), detail.x, y, small_cap,
                   ink.quiet, TextAlign::left, MenuFace::text);
    const double value = std::min(award.target, progress_.value(award.metric));
    const std::string amount =
        award.metric == ProgressMetric::play_seconds
            ? std::format("{:.1f} / {:.0f} {}", value / 3600.0, award.target / 3600.0,
                          award.target == 3600.0 ? "hour played" : "hours played")
            : std::format("{:.0f} / {:.0f}", value, award.target);
    const float amount_cap = menu_cap(menu_cap_medium);
    const float bar_height = std::max(4.0F, std::round(8.0F * unit));
    const float amount_y = detail.y + detail.height - amount_cap - bar_height - 14.0F * unit;
    menu_label(amount, detail.x, amount_y, amount_cap, ink.text, TextAlign::left, MenuFace::text);
    const Rect bar{detail.x, amount_y + amount_cap + 12.0F * unit, detail.width, bar_height};
    fill(bar, {0U, 0U, 0U, 255U});
    fill({bar.x, bar.y,
          bar.width * static_cast<float>(unlocked ? 1.0 : value / std::max(award.target, 1.0)),
          bar.height},
         unlocked ? gold : ink.accent);

    const float footer_y = area.y + area.height - pad - button_height;
    menu_plate_button({body.x, footer_y, 160.0F * unit, button_height},
                      retail_text(RetailString::Back, menu_generation()), false, HitKind::back, 0U);
    menu_label_in(std::format("{} of {} unlocked   -   {} played   -   {:.0f} {}   -   {:.0f} {}",
                              progress_.unlocked_count(), achievement_count,
                              world_playtime(progress_.value(ProgressMetric::play_seconds)),
                              progress_.value(ProgressMetric::all_kills),
                              progress_.value(ProgressMetric::all_kills) == 1.0 ? "kill" : "kills",
                              progress_.value(ProgressMetric::all_crafted),
                              progress_.value(ProgressMetric::all_crafted) == 1.0 ? "craft" : "crafts"),
                  {body.x + 176.0F * unit, footer_y, body.width - 176.0F * unit, button_height}, small_cap,
                  ink.quiet, TextAlign::right, MenuFace::text);
}

// Add-ons and credits: a heading, a few lines, Back.
Rect draw_menu_page(std::string_view heading, std::string_view subtitle)
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect area = draw_menu_table_screen(heading);
    const float pad = 18.0F * unit;
    const float button_height = menu_cap(34.0F);
    menu_label(menu_fitted(std::string{subtitle}, area.width - pad * 2.0F, menu_cap(14.0F), MenuFace::text),
               area.x + pad, area.y + pad, menu_cap(14.0F), ink.quiet, TextAlign::left, MenuFace::text);
    menu_plate_button({area.x + pad, area.y + area.height - pad - button_height, 160.0F * unit,
                       button_height},
                      retail_text(RetailString::Back, menu_generation()), false, HitKind::back, 0U);
    const float top = area.y + pad + menu_cap(14.0F) + 22.0F * unit;
    return {area.x + pad, top, area.width - pad * 2.0F,
            area.y + area.height - pad - button_height - 12.0F * unit - top};
}

// A path in one line no wider than `width`: its end names the folder, so it
// is shortened from the front ("...\Mods\wilderness"), after the first
// `keep` characters (a "Folder: " prefix).
[[nodiscard]] std::string menu_path_fitted(std::string path, std::size_t keep, float width, float cap)
{
    while (path.size() > keep + 4U && menu_width(path, cap, MenuFace::text) > width)
    {
        const std::size_t start = path.compare(keep, 3U, "...") == 0 ? keep + 4U : keep + 1U;
        const std::size_t cut = path.find_first_of("\\/", start);
        if (cut == std::string::npos)
            break;
        path = path.substr(0U, keep) + "..." + path.substr(cut);
    }
    return menu_fitted(std::move(path), width, cap, MenuFace::text);
}

// The Add-Ons screen (app_mod_addons.inc has its model): a list on the left -
// the built-in Xbox Avatars rows, every installed mod with its version, kind
// and state, Open Mods Folder, and the rows mods add (settings pages, key
// bindings) - and on the right what the focused row is: what it does, when a
// change applies, what it means for multiplayer and what it cannot run with.
// Every row is a menu row, so keys, pad and pointer reach all of them; the
// list scrolls with the focus. Under it, what the last change did.
void draw_addons_page()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect area = draw_menu_table_screen("Add-Ons");
    const float pad = 18.0F * unit;
    const float text_cap = menu_cap(14.0F);
    const float small_cap = menu_cap(menu_cap_small);
    const float row_cap = menu_cap(15.0F);
    const float heading_cap = menu_cap(menu_cap_medium);
    const float button_height = menu_cap(34.0F);

    std::string intro = "Choose an add-on to turn it on or off. Client add-ons apply at once, server add-ons in "
                        "the next world.";
    bool intro_warning = false;
    if (const std::string locked = addons_locked_reason(); !locked.empty())
    {
        intro = locked;
        intro_warning = true;
    }
    else if (!mod_catalog_valid_)
    {
        intro = "Add-ons are blocked: " + mod_catalog_error_;
        intro_warning = true;
    }
    menu_label(menu_fitted(intro, area.width - pad * 2.0F, text_cap, MenuFace::text), area.x + pad, area.y + pad,
               text_cap, intro_warning ? ink.warning : ink.quiet, TextAlign::left, MenuFace::text);
    const float top = area.y + pad + text_cap + 22.0F * unit;
    const float footer_y = area.y + area.height - pad - button_height;
    const Rect body{area.x + pad, top, area.width - pad * 2.0F, std::max(0.0F, footer_y - 16.0F * unit - top)};
    const float list_width = std::floor(body.width * 0.54F);
    const Rect detail{body.x + list_width + 30.0F * unit, body.y, body.width - list_width - 30.0F * unit, body.height};

    // The list as lines: a heading before each group, then its rows.
    const auto &entries = menu_.entries();
    const std::size_t selection = entries.empty() ? 0U : std::min(menu_.selection(), entries.size() - 1U);
    constexpr std::size_t no_entry = static_cast<std::size_t>(-1);
    struct Line final
    {
        float top{};
        float height{};
        std::size_t entry{no_entry};
        int group{};       // 0 Xbox Avatars, 1 installed add-ons, 2 rows mods add
        bool heading{};
    };
    const auto group_of = [](ui::MenuCommand command) {
        return command == ui::MenuCommand::toggle_addon || command == ui::MenuCommand::open_mods_folder ? 1
               : command == ui::MenuCommand::mod_entry                                                ? 2
                                                                                                       : 0;
    };
    const float pitch = menu_cap(30.0F);
    const float heading_height = heading_cap + 18.0F * unit;
    std::vector<Line> lines;
    std::size_t first_addon = no_entry;
    float y = 0.0F;
    for (std::size_t index = 0U; index < entries.size(); ++index)
    {
        const int group = group_of(entries[index].command);
        if (group == 1 && first_addon == no_entry)
            first_addon = index;
        if (index == 0U || group != group_of(entries[index - 1U].command))
        {
            if (!lines.empty())
                y += 12.0F * unit;
            lines.push_back({y, heading_height, no_entry, group, true});
            y += heading_height;
            // No mods yet: a line saying so above Open Mods Folder.
            if (group == 1 && addon_scan_.addons.empty())
            {
                lines.push_back({y, pitch, no_entry, group, false});
                y += pitch;
            }
        }
        lines.push_back({y, pitch, index, group, false});
        y += pitch;
    }
    const float total = y;
    // Scrolled so the focused row, and the heading right above it, show.
    for (std::size_t line = 0U; line < lines.size(); ++line)
    {
        if (lines[line].entry != selection)
            continue;
        const float want = line > 0U && lines[line - 1U].heading ? lines[line - 1U].top : lines[line].top;
        const float bottom = lines[line].top + lines[line].height;
        if (want < addon_scroll_)
            addon_scroll_ = want;
        if (bottom > addon_scroll_ + body.height)
            addon_scroll_ = bottom - body.height;
    }
    addon_scroll_ = std::clamp(addon_scroll_, 0.0F, std::max(0.0F, total - body.height));

    const std::size_t on_count = static_cast<std::size_t>(std::count_if(
        addon_status_.begin(), addon_status_.end(), [](const modding::AddonStatus &status) { return status.on; }));
    const float inset = 12.0F * unit;
    const float state_width = 150.0F * unit;
    const float kind_width = 62.0F * unit;
    for (const Line &line : lines)
    {
        const float line_top = body.y + line.top - addon_scroll_;
        if (line_top < body.y - 0.5F || line_top + line.height > body.y + body.height + 0.5F)
            continue;
        if (line.heading)
        {
            static constexpr std::array<std::string_view, 3> headings{"Xbox Avatars", "Installed Add-Ons",
                                                                      "Add-On Settings"};
            const float cap_top = line_top + 4.0F * unit;
            menu_label(headings[static_cast<std::size_t>(line.group)], body.x, cap_top, heading_cap, ink.heading,
                       TextAlign::left, MenuFace::display);
            if (line.group == 1 && !addon_scan_.addons.empty())
                menu_label(std::format("{} of {} on", on_count, addon_scan_.addons.size()), body.x + list_width - inset,
                           cap_top + heading_cap - small_cap, small_cap, ink.quiet, TextAlign::right, MenuFace::text);
            menu_rule(body.x, cap_top + heading_cap + 8.0F * unit, list_width, 0.22F);
            continue;
        }
        if (line.entry == no_entry)
        {
            menu_label_in("Nothing installed yet.", {body.x + inset, line_top, list_width - inset * 2.0F, pitch},
                          text_cap, ink.quiet, TextAlign::left, MenuFace::text);
            continue;
        }
        const ui::MenuEntry &item = entries[line.entry];
        const Rect row{body.x, line_top, list_width, pitch - 3.0F * unit};
        const bool focused = line.entry == selection;
        if (focused)
            menu_focus_band(row);
        hits_.push_back({row, HitKind::menu_item, line.entry});
        const SDL_Color focus = menu_focus_color();
        const Rect state_box{row.x + row.width - inset - state_width, row.y, state_width, row.height};
        const std::size_t addon_row = first_addon == no_entry ? no_entry : line.entry - first_addon;
        if (item.command == ui::MenuCommand::toggle_addon && addon_row < addon_row_mods_.size() &&
            addon_row_mods_[addon_row] < addon_scan_.addons.size())
        {
            const std::size_t index = addon_row_mods_[addon_row];
            const modding::InstalledAddon &addon = addon_scan_.addons[index];
            const AddonRowState state = addon_row_state(index);
            const bool trouble = state == AddonRowState::not_loaded || state == AddonRowState::script_error;
            const std::string_view state_text = state == AddonRowState::on           ? "On"
                                                : state == AddonRowState::off        ? "Off"
                                                : state == AddonRowState::not_loaded ? "Not Loaded"
                                                                                     : "Script Error";
            // Off rows are greyed as the originals grey a locked mode, though
            // they stay selectable: the state column says what they are.
            const SDL_Color name_color = focused ? focus : state == AddonRowState::off ? ink.disabled : ink.row;
            const SDL_Color state_color = trouble                       ? ink.warning
                                          : focused                     ? focus
                                          : state == AddonRowState::on ? ink.row
                                                                        : ink.quiet;
            const std::string version = addon.usable() ? addon.mod.descriptor.version : std::string{};
            const float version_room =
                version.empty() ? 0.0F : menu_width(version, small_cap, MenuFace::text) + 12.0F * unit;
            const float name_room = row.width - inset * 2.0F - state_width - kind_width - 10.0F * unit;
            const float name_width =
                menu_label_in(addon.info.name, {row.x + inset, row.y, std::max(0.0F, name_room - version_room), row.height},
                              row_cap, name_color, TextAlign::left, MenuFace::display);
            if (!version.empty())
                menu_label_in(version, {row.x + inset + name_width + 10.0F * unit, row.y, version_room, row.height},
                              small_cap, state == AddonRowState::off && !focused ? ink.disabled : ink.quiet,
                              TextAlign::left, MenuFace::text);
            if (addon.usable())
                menu_label_in(addon.mod.descriptor.scope == net::ModScope::client ? "Client"
                              : addon.mod.descriptor.scope == net::ModScope::core ? "Core"
                                                                                  : "Server",
                              {state_box.x - kind_width, row.y, kind_width, row.height}, small_cap, ink.quiet,
                              TextAlign::left, MenuFace::text);
            menu_label_in(state_text, state_box, row_cap, state_color, TextAlign::right, MenuFace::text);
            continue;
        }
        menu_label_in(item.label, {row.x + inset, row.y, row.width - inset * 2.0F - state_width, row.height}, row_cap,
                      focused ? focus : ink.row, TextAlign::left, MenuFace::display);
        const std::string value = item.command == ui::MenuCommand::mod_entry ? std::string{"Open"}
                                                                             : menu_value_case(xbox_avatar_setting_value(item.command));
        if (!value.empty())
            menu_label_in(value, state_box, row_cap, focused ? focus : ink.row, TextAlign::right, MenuFace::text);
    }
    if (total > body.height)
        menu_scroll_mark(body.x + list_width + 6.0F * unit, body.y, body.height, body.height / total,
                         addon_scroll_ / std::max(1.0F, total - body.height));
    fill({detail.x - 15.0F * unit, detail.y, std::max(1.0F, std::round(unit)), detail.height}, {255U, 255U, 255U, 40U});

    // What the focused row is.
    const float title_cap = menu_cap(menu_cap_text);
    const float detail_bottom = detail.y + detail.height;
    float dy = detail.y + 2.0F * unit;
    const auto paragraph = [&](std::string_view text, float cap, SDL_Color color, float reserve = 0.0F,
                               float after = 18.0F) {
        if (text.empty() || dy + cap > detail_bottom - reserve)
            return;
        dy += menu_paragraph(text, {detail.x, dy, detail.width, detail_bottom - reserve - dy}, cap, color,
                             MenuFace::text) +
              after * unit;
    };
    const auto title = [&](std::string_view text) {
        menu_label(menu_fitted(std::string{text}, detail.width, title_cap, MenuFace::display), detail.x, dy, title_cap,
                   ink.heading, TextAlign::left, MenuFace::display);
        dy += title_cap + 14.0F * unit;
    };
    if (!entries.empty())
    {
        const ui::MenuEntry &item = entries[selection];
        const std::size_t addon_row = first_addon == no_entry || selection < first_addon ? no_entry : selection - first_addon;
        if (item.command == ui::MenuCommand::toggle_addon && addon_row < addon_row_mods_.size() &&
            addon_row_mods_[addon_row] < addon_scan_.addons.size())
        {
            const std::size_t index = addon_row_mods_[addon_row];
            const modding::InstalledAddon &addon = addon_scan_.addons[index];
            const AddonDetail about = addon_detail(index);
            const float folder_room = small_cap + 12.0F * unit;
            title(addon.info.name);
            paragraph(about.facts, small_cap, ink.quiet, folder_room, about.author.empty() ? 18.0F : 8.0F);
            paragraph(about.author, small_cap, ink.quiet, folder_room);
            paragraph(about.state, text_cap, about.state_warning ? ink.warning : ink.row, folder_room);
            paragraph(addon.info.description, text_cap, ink.text, folder_room);
            paragraph(about.multiplayer, small_cap, ink.quiet, folder_room, 12.0F);
            paragraph(about.notes, small_cap, ink.quiet, folder_room);
            menu_label(menu_path_fitted(about.folder, 8U, detail.width, small_cap), detail.x,
                       detail_bottom - small_cap, small_cap, ink.quiet, TextAlign::left, MenuFace::text);
        }
        else if (item.command == ui::MenuCommand::open_mods_folder)
        {
            const std::u8string user = modding::user_mod_root().u8string();
            const std::u8string bundled = modding::bundled_mod_root(application_directory()).u8string();
            const auto path_line = [&](const std::u8string &path) {
                if (dy + small_cap > detail_bottom)
                    return;
                menu_label(menu_path_fitted(std::string{path.begin(), path.end()}, 0U, detail.width, small_cap),
                           detail.x, dy, small_cap, ink.quiet, TextAlign::left, MenuFace::text);
                dy += small_cap + 18.0F * unit;
            };
            title(item.label);
            paragraph("Opens your Mods folder in Explorer, made if it is missing:", text_cap, ink.text, 0.0F, 8.0F);
            path_line(user);
            paragraph("Copy an add-on's whole folder there (the folder with openminerz.mod in it), then come back "
                      "to the game: the list updates by itself. Add-ons you install start on.",
                      text_cap, ink.text);
            paragraph("The examples bundled with the game start off. To change one, copy it to your Mods folder "
                      "first: your copy replaces the bundled one. They are in:",
                      small_cap, ink.quiet, 0.0F, 8.0F);
            path_line(bundled);
        }
        else if (item.command == ui::MenuCommand::mod_entry)
        {
            title(item.label);
            paragraph(item.description, text_cap, ink.text);
        }
        else
        {
            // Xbox Avatars: built in, presentation only (app_xbox_avatar.inc).
            title("Xbox Avatars");
            paragraph(item.description, text_cap, ink.text);
            paragraph(xbox_avatar_status_text(), text_cap, ink.quiet);
        }
    }

    menu_plate_button({body.x, footer_y, 160.0F * unit, button_height},
                      retail_text(RetailString::Back, menu_generation()), false, HitKind::back, 0U);
    if (!addon_notice_.empty())
        menu_paragraph(addon_notice_,
                       {body.x + 176.0F * unit, footer_y + std::max(0.0F, (button_height - menu_line(small_cap) - small_cap) * 0.5F),
                        body.width - 176.0F * unit, button_height},
                       small_cap, addon_notice_warning_ ? ink.warning : ink.text, MenuFace::text);
}

// A world whose terrain comes from a mod that is off or missing: what is
// wrong, and Turn On and Open (or Open Mods Folder) beside Cancel.
void draw_addon_world_prompt()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const AddonWorldPrompt &prompt = *addon_world_prompt_;
    const Rect panel = draw_menu_dialog("World Needs an Add-On", 660.0F, 262.0F);
    const float text_cap = menu_cap(15.0F);
    const float button_height = menu_cap(40.0F);
    const float title_bottom = panel.y + 16.0F * unit + menu_cap(menu_cap_text) + 30.0F * unit;
    menu_paragraph(prompt.body,
                   {panel.x + 24.0F * unit, title_bottom, panel.width - 48.0F * unit,
                    panel.y + panel.height - 20.0F * unit - button_height - 14.0F * unit - title_bottom},
                   text_cap, ink.text, MenuFace::text);
    std::vector<std::string_view> labels;
    if (prompt.action == AddonPromptAction::turn_on)
        labels = {"Turn On and Open", "Cancel"};
    else if (prompt.action == AddonPromptAction::open_folder)
        labels = {"Open Mods Folder", "Back"};
    else
        labels = {"Back"};
    const float gap = 12.0F * unit;
    const float width = (panel.width - 48.0F * unit - gap * static_cast<float>(labels.size() - 1U)) /
                        static_cast<float>(labels.size());
    for (std::size_t button = 0U; button < labels.size(); ++button)
    {
        const Rect rect{panel.x + 24.0F * unit + static_cast<float>(button) * (width + gap),
                        panel.y + panel.height - 20.0F * unit - button_height, width, button_height};
        addon_prompt_buttons_[button] = rect;
        // One button is Back: its hit cancels.
        menu_plate_button(rect, labels[button], button == addon_prompt_focus_, HitKind::addon_prompt,
                          labels.size() == 1U ? 1U : button, menu_cap_medium);
    }
}

void draw_credits_page()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect body = draw_menu_page("Credits", "CastleMiner Z, rebuilt for modern PCs.");
    const std::array<std::pair<std::string_view, std::string_view>, 3> lines{{
        {"Direction", "KikoTs and the OpenMinerZ community"},
        {"Source Game", "CastleMiner Z by DigitalDNA Games"},
        {"Runtime", "SDL3, FreeType, CMake, C++20"},
    }};
    const float cap = menu_cap(menu_cap_medium);
    float y = body.y + 6.0F * unit;
    for (const auto &[label, value] : lines)
    {
        menu_label(label, body.x, y + (cap - menu_cap(menu_cap_small)), menu_cap(menu_cap_small),
                   ink.quiet, TextAlign::left, MenuFace::display);
        menu_label(menu_fitted(std::string{value}, body.width - 150.0F * unit, cap, MenuFace::text),
                   body.x + 150.0F * unit, y, cap, ink.text, TextAlign::left, MenuFace::text);
        y += cap + 24.0F * unit;
    }
}

// --------------------------------------------------------------- dialogs

// "Are you sure you want to quit?", as 1.9.9.8 asks it.
void draw_exit_dialog()
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const RetailGeneration generation = menu_generation();
    const Rect panel = draw_menu_dialog(retail_text(RetailString::Quit_Game, generation), 560.0F, 230.0F);
    const float text_cap = menu_cap(menu_cap_medium);
    const float title_bottom = panel.y + 16.0F * unit + menu_cap(menu_cap_text) + 30.0F * unit;
    menu_paragraph(retail_text(RetailString::Are_you_sure_you_want_to_quit, generation),
                   {panel.x + 24.0F * unit, title_bottom, panel.width - 48.0F * unit, menu_line(text_cap) * 2.0F},
                   text_cap, ink.text, MenuFace::text);
    const auto &entries = menu_.entries();
    const float button_height = menu_cap(40.0F);
    const float gap = 12.0F * unit;
    const float button_width =
        (panel.width - 48.0F * unit - gap * static_cast<float>(std::max<std::size_t>(entries.size(), 1U) - 1U)) /
        static_cast<float>(std::max<std::size_t>(entries.size(), 1U));
    for (std::size_t index = 0U; index < entries.size(); ++index)
        menu_plate_button({panel.x + 24.0F * unit + static_cast<float>(index) * (button_width + gap),
                           panel.y + panel.height - 20.0F * unit - button_height, button_width,
                           button_height},
                          entries[index].label, index == menu_.selection(), HitKind::menu_item, index,
                          menu_cap_medium);
}

// A dialog with one line to type into (PCKeyboardInputScreen): the prompt,
// the field with its caret, how much room is left and what the keys do.
void draw_menu_text_dialog(std::string_view title, std::string_view prompt, std::string_view shown,
                           std::size_t used, std::size_t limit)
{
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect panel = draw_menu_dialog(title, 620.0F, 250.0F);
    const float text_cap = menu_cap(14.0F);
    const float field_cap = menu_cap(menu_cap_medium);
    const float small_cap = menu_cap(menu_cap_small);
    float y = panel.y + 16.0F * unit + menu_cap(menu_cap_text) + 28.0F * unit;
    menu_label(menu_fitted(std::string{prompt}, panel.width - 48.0F * unit, text_cap, MenuFace::text),
               panel.x + 24.0F * unit, y, text_cap, ink.text, TextAlign::left, MenuFace::text);
    y += text_cap + 14.0F * unit;
    const Rect field{panel.x + 24.0F * unit, y, panel.width - 48.0F * unit, field_cap + 26.0F * unit};
    fill(field, {0U, 0U, 0U, 255U});
    stroke(field, {255U, 255U, 255U, 220U}, std::max(1.0F, std::round(unit)));
    const std::string count = std::format("{}/{}", used, limit);
    const float count_width = menu_width(count, small_cap, MenuFace::text) + 12.0F * unit;
    // Keep the end of a long entry in view.
    std::string visible{shown};
    const float room = field.width - 24.0F * unit - count_width;
    while (!visible.empty() && menu_width(visible + "|", field_cap, MenuFace::text) > room)
    {
        std::size_t next = 1U;
        while (next < visible.size() && (static_cast<unsigned char>(visible[next]) & 0xC0U) == 0x80U)
            ++next;
        visible.erase(0U, next);
    }
    const bool caret = settings_.reduced_motion || std::fmod(elapsed_, 1.0F) < 0.5F;
    menu_label_in(visible + (caret ? "|" : ""), {field.x + 12.0F * unit, field.y, room, field.height},
                  field_cap, ink.text, TextAlign::left, MenuFace::text, false);
    menu_label_in(count, {field.x + field.width - count_width, field.y, count_width - 8.0F * unit, field.height},
                  small_cap, ink.quiet, TextAlign::right, MenuFace::text, false);
    const bool pad = gamepad_input_active_;
    const std::array<MenuKeyHint, 2> hints{
        {{pad ? "A" : "Enter", "Accept"}, {pad ? "B" : "Esc", "Cancel"}}};
    menu_key_hints(hints, panel.x + 24.0F * unit, panel.y + panel.height - 20.0F * unit - small_cap,
                   panel.width - 48.0F * unit);
}

// ------------------------------------------------------------------- state

std::size_t settings_first_item_{};
ui::SettingsTab settings_scroll_tab_{ui::SettingsTab::display};
bool menu_font_notice_{};
float menu_cap_ratio_{};
float menu_focus_since_{};
std::shared_ptr<SDL_Texture> menu_backdrop_;
std::shared_ptr<SDL_Texture> menu_backdrop_soft_;
std::future<MenuBackdropPixels> menu_backdrop_loading_;
std::filesystem::path menu_backdrop_path_;
bool menu_backdrop_begun_{};
bool menu_backdrop_wide_{};
float menu_backdrop_since_{};
bool menu_sound_primed_{};
bool menu_sound_dialog_{};
bool menu_focus_moved_by_input_{};
ui::MenuScreen menu_sound_screen_{ui::MenuScreen::main};
std::size_t menu_sound_selection_{};
ui::SettingsTab menu_sound_tab_{ui::SettingsTab::display};
std::size_t menu_sound_serial_{};
