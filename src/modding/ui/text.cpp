// Rich text, its layout, and localization (openminerz/modding/ui.hpp).

#include "openminerz/modding/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>

namespace openminerz::modding::ui
{
namespace
{

// Bytes in the UTF-8 sequence that starts at `text[at]` (1 for a stray byte).
[[nodiscard]] std::size_t sequence_length(std::string_view text, std::size_t at) noexcept
{
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = lead < 0x80U ? 1U : (lead & 0xE0U) == 0xC0U ? 2U : (lead & 0xF0U) == 0xE0U ? 3U
                                         : (lead & 0xF8U) == 0xF0U ? 4U : 1U;
    if (at + length > text.size())
        return 1U;
    for (std::size_t index = 1U; index < length; ++index)
        if ((static_cast<unsigned char>(text[at + index]) & 0xC0U) != 0x80U)
            return 1U;
    return length;
}

struct MarkupState final
{
    char tag{};
    std::optional<Color> color;
    float size{1.0F};
    std::optional<Face> face;
};

void push_text(std::vector<TextRun> &runs, const MarkupState &state, std::string_view text)
{
    if (text.empty())
        return;
    if (!runs.empty() && runs.back().color == state.color && runs.back().size == state.size &&
        runs.back().face == state.face)
    {
        runs.back().text.append(text);
        return;
    }
    runs.push_back({std::string{text}, state.color, state.size, state.face});
}

[[nodiscard]] std::string value_text(const Value &value)
{
    switch (value.type())
    {
    case ValueType::string:
        return value.as_string();
    case ValueType::integer:
        return std::to_string(std::get<std::int64_t>(value.storage()));
    case ValueType::number: {
        const double number = std::get<double>(value.storage());
        if (std::isfinite(number) && std::trunc(number) == number && std::fabs(number) < 1e15)
            return std::to_string(static_cast<std::int64_t>(number));
        return std::format("{}", number);
    }
    case ValueType::boolean:
        return value.as_bool() ? "true" : "false";
    default:
        return {};
    }
}

[[nodiscard]] std::string trim(std::string_view text)
{
    std::size_t first = 0U;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t'))
        ++first;
    std::size_t last = text.size();
    while (last > first && (text[last - 1U] == ' ' || text[last - 1U] == '\t' || text[last - 1U] == '\r'))
        --last;
    return std::string{text.substr(first, last - first)};
}

} // namespace

std::vector<TextRun> parse_markup(std::string_view text)
{
    std::vector<TextRun> runs;
    std::vector<MarkupState> stack{MarkupState{}};
    std::size_t at = 0U;
    std::size_t literal = 0U;
    const auto flush = [&](std::size_t end) {
        push_text(runs, stack.back(), text.substr(literal, end - literal));
    };
    while (at < text.size())
    {
        if (text[at] != '[')
        {
            ++at;
            continue;
        }
        if (at + 1U < text.size() && text[at + 1U] == '[')
        {
            flush(at + 1U); // keep one '['
            at += 2U;
            literal = at;
            continue;
        }
        const std::size_t close = text.find(']', at);
        if (close == std::string_view::npos || close - at > 32U)
        {
            ++at;
            continue;
        }
        const std::string_view tag = text.substr(at + 1U, close - at - 1U);
        bool handled = false;
        if (tag.size() == 2U && tag[0] == '/')
        {
            if (stack.size() > 1U && stack.back().tag == tag[1])
            {
                flush(at);
                stack.pop_back();
                handled = true;
            }
        }
        else if (tag.size() > 2U && tag[1] == '=' && stack.size() < 16U)
        {
            MarkupState next = stack.back();
            next.tag = tag[0];
            const std::string_view argument = tag.substr(2U);
            if (tag[0] == 'c')
            {
                if (const std::optional<Color> color = parse_color(Value{argument}))
                {
                    next.color = color;
                    handled = true;
                }
            }
            else if (tag[0] == 's')
            {
                char *end{};
                const std::string number{argument};
                const float size = std::strtof(number.c_str(), &end);
                if (end != nullptr && *end == '\0' && std::isfinite(size) && size >= 0.25F && size <= 4.0F)
                {
                    next.size = size;
                    handled = true;
                }
            }
            else if (tag[0] == 'f' && (argument == "display" || argument == "text"))
            {
                next.face = argument == "display" ? Face::display : Face::text;
                handled = true;
            }
            if (handled)
            {
                flush(at);
                stack.push_back(next);
            }
        }
        if (handled)
        {
            at = close + 1U;
            literal = at;
        }
        else
            ++at;
    }
    flush(text.size());
    return runs;
}

std::vector<TextRun> runs_from_value(const Value &value)
{
    if (value.is_nil())
        return {};
    if (value.type() == ValueType::string)
        return parse_markup(value.as_string());
    if (value.is_number() || value.type() == ValueType::boolean)
        return parse_markup(value_text(value));
    const bool empty_map = value.type() == ValueType::map && value.as_map().empty();
    if (value.type() != ValueType::array && !empty_map)
        throw ScriptError{"text must be a string or an array of runs"};
    std::vector<TextRun> runs;
    if (empty_map)
        return runs;
    for (const Value &part : value.as_array())
    {
        if (part.type() == ValueType::string)
        {
            for (TextRun &run : parse_markup(part.as_string()))
                runs.push_back(std::move(run));
            continue;
        }
        if (part.type() != ValueType::map)
            throw ScriptError{"a text run is a string or {text=, color=, size=, face=}"};
        TextRun run;
        const Value *text = part.field("text");
        if (text == nullptr || text->type() != ValueType::string)
            throw ScriptError{"a text run needs text"};
        run.text = text->as_string();
        if (const Value *color = part.field("color"); color != nullptr && !color->is_nil())
        {
            run.color = parse_color(*color);
            if (!run.color)
                throw ScriptError{"a text run's color is not a colour"};
        }
        if (const Value *size = part.field("size"); size != nullptr && !size->is_nil())
        {
            const double scale = size->as_number();
            if (!std::isfinite(scale) || scale < 0.25 || scale > 4.0)
                throw ScriptError{"a text run's size is 0.25..4"};
            run.size = static_cast<float>(scale);
        }
        if (const Value *face = part.field("face"); face != nullptr && !face->is_nil())
        {
            const std::string &name = face->as_string();
            if (name != "display" && name != "text")
                throw ScriptError{"a text run's face is \"display\" or \"text\""};
            run.face = name == "display" ? Face::display : Face::text;
        }
        runs.push_back(std::move(run));
    }
    return runs;
}

std::string plain_text(std::span<const TextRun> runs)
{
    std::string result;
    for (const TextRun &run : runs)
        result += run.text;
    return result;
}

TextBlock layout_text(std::span<const TextRun> runs, Face face, float cap, Color color,
                      float max_width, std::size_t max_lines, Align align, const TextMeasure &measure)
{
    TextBlock block;
    if (!std::isfinite(cap) || cap <= 0.0F || runs.empty())
        return block;
    struct Piece final
    {
        std::size_t run{};
        std::string text;
    };
    struct Line final
    {
        std::vector<Piece> pieces;
        float width{};
        float cap{};
    };
    std::vector<Line> lines(1U);
    const auto run_face = [&](std::size_t run) { return runs[run].face.value_or(face); };
    const auto run_cap = [&](std::size_t run) { return cap * runs[run].size; };
    const auto width_of = [&](std::size_t run, std::string_view text) {
        return text.empty() ? 0.0F : measure.width(run_face(run), run_cap(run), text);
    };
    const auto line_width = [&](const Line &line) {
        float width = 0.0F;
        for (const Piece &piece : line.pieces)
            width += width_of(piece.run, piece.text);
        return width;
    };
    const bool wrapping = max_width > 0.0F && std::isfinite(max_width);
    const auto add = [&](std::size_t run, std::string_view word, bool space) {
        Line &line = lines.back();
        if (!line.pieces.empty() && line.pieces.back().run == run)
        {
            if (space)
                line.pieces.back().text.push_back(' ');
            line.pieces.back().text.append(word);
        }
        else
        {
            std::string text = space ? std::string{" "} : std::string{};
            text.append(word);
            line.pieces.push_back({run, std::move(text)});
        }
        line.width = line_width(line);
        line.cap = std::max(line.cap, run_cap(run));
    };
    // A word goes onto the current line when it fits, else onto a new one; a
    // word wider than a whole line is cut between characters.
    const auto place = [&](std::size_t run, std::string_view word, bool space) {
        while (!word.empty())
        {
            const Line &line = lines.back();
            const bool spaced = space && !line.pieces.empty();
            const float needed = (spaced ? width_of(run, " ") : 0.0F) + width_of(run, word);
            if (!wrapping || line.width + needed <= max_width)
            {
                add(run, word, spaced);
                return;
            }
            if (!line.pieces.empty())
            {
                lines.emplace_back();
                space = false;
                continue;
            }
            std::size_t keep = sequence_length(word, 0U);
            while (keep < word.size())
            {
                const std::size_t next = keep + sequence_length(word, keep);
                if (width_of(run, word.substr(0U, next)) > max_width)
                    break;
                keep = next;
            }
            add(run, word.substr(0U, keep), false);
            word.remove_prefix(keep);
            if (!word.empty())
                lines.emplace_back();
            space = false;
        }
    };
    for (std::size_t run = 0U; run < runs.size(); ++run)
    {
        const std::string_view text = runs[run].text;
        bool space = false;
        std::size_t at = 0U;
        while (at < text.size())
        {
            const char c = text[at];
            if (c == '\n')
            {
                lines.emplace_back();
                space = false;
                ++at;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r')
            {
                space = true;
                ++at;
                continue;
            }
            std::size_t end = at;
            while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\t' &&
                   text[end] != '\r')
                ++end;
            // A space that ended the previous run belongs before this word.
            if (!space && at == 0U && run > 0U)
            {
                const std::string_view previous = runs[run - 1U].text;
                space = !previous.empty() && (previous.back() == ' ' || previous.back() == '\t');
            }
            place(run, text.substr(at, end - at), space);
            space = false;
            at = end;
        }
    }
    // An empty line keeps the height of the base size.
    for (Line &line : lines)
        if (line.cap <= 0.0F)
            line.cap = cap;
    if (max_lines > 0U && lines.size() > max_lines)
    {
        lines.resize(max_lines);
        block.truncated = true;
        Line &last = lines.back();
        const float limit = wrapping ? max_width : std::numeric_limits<float>::max();
        // Shorten the last line until it and an ellipsis fit.
        while (!last.pieces.empty())
        {
            Piece &piece = last.pieces.back();
            if (line_width(last) + width_of(piece.run, "...") <= limit || piece.text.empty())
                break;
            std::size_t cut = piece.text.size() - 1U;
            while (cut > 0U && (static_cast<unsigned char>(piece.text[cut]) & 0xC0U) == 0x80U)
                --cut;
            piece.text.resize(cut);
            if (piece.text.empty())
                last.pieces.pop_back();
        }
        if (last.pieces.empty())
            last.pieces.push_back({0U, std::string{}});
        last.pieces.back().text += "...";
        last.width = line_width(last);
    }
    float top = 0.0F;
    for (std::size_t index = 0U; index < lines.size(); ++index)
    {
        const Line &line = lines[index];
        float x = 0.0F;
        if (wrapping && align != Align::left)
            x = std::max(0.0F, max_width - line.width) * (align == Align::center ? 0.5F : 1.0F);
        for (const Piece &piece : line.pieces)
        {
            const float piece_cap = run_cap(piece.run);
            block.pieces.push_back({piece.text, x, top + (line.cap - piece_cap), piece_cap,
                                    run_face(piece.run), runs[piece.run].color.value_or(color)});
            x += width_of(piece.run, piece.text);
        }
        block.width = std::max(block.width, line.width);
        block.height = top + line.cap;
        if (index + 1U < lines.size())
            top += line_pitch(std::max(line.cap, lines[index + 1U].cap));
    }
    block.lines = lines.size();
    return block;
}

// ---- Localization ----------------------------------------------------------

bool parse_lang_file(std::string_view text, StringTable &table, std::string &error)
{
    if (text.size() > max_lang_bytes)
    {
        error = "the file is larger than 1 MiB";
        return false;
    }
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3U);
    std::size_t number = 0U;
    std::size_t at = 0U;
    while (at <= text.size())
    {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = trim(text.substr(at, end - at));
        ++number;
        at = end + 1U;
        if (line.empty() || line[0] == '#' || line[0] == ';')
        {
            if (end == text.size())
                break;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            error = std::format("line {}: expected key = value", number);
            return false;
        }
        const std::string key = trim(std::string_view{line}.substr(0U, equals));
        if (key.empty() || key.size() > 128U ||
            !std::ranges::all_of(key, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-';
            }))
        {
            error = std::format("line {}: '{}' is not a key", number, key);
            return false;
        }
        const std::string raw = trim(std::string_view{line}.substr(equals + 1U));
        std::string value;
        value.reserve(raw.size());
        for (std::size_t index = 0U; index < raw.size(); ++index)
        {
            if (raw[index] == '\\' && index + 1U < raw.size())
            {
                const char next = raw[index + 1U];
                if (next == 'n' || next == 't' || next == '\\')
                {
                    value.push_back(next == 'n' ? '\n' : next == 't' ? '\t' : '\\');
                    ++index;
                    continue;
                }
            }
            value.push_back(raw[index]);
        }
        if (table.size() >= max_lang_entries && !table.contains(key))
        {
            error = std::format("more than {} entries", max_lang_entries);
            return false;
        }
        table.insert_or_assign(key, std::move(value));
        if (end == text.size())
            break;
    }
    return true;
}

std::string format_text(std::string_view pattern, const Value &args)
{
    std::string result;
    result.reserve(pattern.size());
    for (std::size_t at = 0U; at < pattern.size(); ++at)
    {
        const char c = pattern[at];
        if (c == '{' && at + 1U < pattern.size() && pattern[at + 1U] == '{')
        {
            result.push_back('{');
            ++at;
            continue;
        }
        if (c != '{')
        {
            result.push_back(c);
            continue;
        }
        const std::size_t close = pattern.find('}', at);
        if (close == std::string_view::npos || close - at > 64U)
        {
            result.push_back(c);
            continue;
        }
        const std::string_view name = pattern.substr(at + 1U, close - at - 1U);
        const Value *found = nullptr;
        if (args.type() == ValueType::array && name.size() == 1U && name[0] >= '1' && name[0] <= '9')
        {
            const auto index = static_cast<std::size_t>(name[0] - '1');
            if (index < args.as_array().size())
                found = &args.as_array()[index];
        }
        else if (args.type() == ValueType::map)
            found = args.field(name);
        else if (name == "1" && !args.is_nil() && args.type() != ValueType::array)
            found = &args;
        const std::string replacement = found != nullptr ? value_text(*found) : std::string{};
        if (found == nullptr || (replacement.empty() && found->type() != ValueType::string))
        {
            result.append(pattern.substr(at, close - at + 1U));
        }
        else
            result += replacement;
        at = close;
    }
    return result;
}

std::vector<std::string> language_fallbacks(std::string_view language)
{
    std::string code;
    for (const char c : language.substr(0U, 16U))
    {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0)
            code.push_back(static_cast<char>(std::tolower(byte)));
        else if (c == '-' || c == '_')
            code.push_back('-');
    }
    std::vector<std::string> chain;
    if (!code.empty())
        chain.push_back(code);
    if (const std::size_t dash = code.find('-'); dash != std::string::npos && dash > 0U)
        chain.push_back(code.substr(0U, dash));
    if (std::ranges::find(chain, "en") == chain.end())
        chain.emplace_back("en");
    return chain;
}

void Localization::set_language(std::string_view code)
{
    const std::vector<std::string> chain = language_fallbacks(code);
    language_ = chain.front();
}

void Localization::add_table(std::string_view mod, std::string_view language, StringTable table)
{
    tables_[std::string{mod}].insert_or_assign(std::string{language}, std::move(table));
}

void Localization::forget(std::string_view mod)
{
    if (const auto found = tables_.find(mod); found != tables_.end())
        tables_.erase(found);
}

void Localization::clear() noexcept
{
    tables_.clear();
    errors_.clear();
}

const StringTable *Localization::table(std::string_view mod, std::string_view language)
{
    auto &languages = tables_[std::string{mod}];
    if (const auto found = languages.find(language); found != languages.end())
        return &found->second;
    StringTable table;
    if (reader_)
    {
        const std::string path = std::format("lang/{}.lang", language);
        if (const std::optional<std::string> text = reader_(mod, path))
        {
            std::string error;
            if (!parse_lang_file(*text, table, error))
            {
                table.clear();
                if (errors_.size() < 64U)
                    errors_.push_back(std::format("{}: {}: {}", mod, path, error));
            }
        }
    }
    return &languages.emplace(std::string{language}, std::move(table)).first->second;
}

std::string Localization::text(std::string_view mod, std::string_view key, const Value &args)
{
    for (const std::string &language : language_fallbacks(language_))
        if (const StringTable *strings = table(mod, language))
            if (const auto found = strings->find(key); found != strings->end())
                return format_text(found->second, args);
    return std::string{key};
}

} // namespace openminerz::modding::ui
