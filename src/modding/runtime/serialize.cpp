// util.serialize / util.deserialize and the mod storage format: compact JSON
// with two tagged objects, {"$vector":[x,y,z]} and {"$handle":[kind,id]}. A map
// key that starts with '$' is written with one more '$' so it can never be
// mistaken for a tag.

#include "openminerz/modding/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>

namespace openminerz::modding
{
namespace
{

constexpr int maximum_depth = 64;

void write_string(std::string &out, std::string_view text)
{
    out += '"';
    for (const char character : text)
    {
        const auto byte = static_cast<unsigned char>(character);
        switch (character)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20U)
                out += std::format("\\u{:04x}", byte);
            else
                out += character;
        }
    }
    out += '"';
}

void write_number(std::string &out, double number)
{
    std::string text = std::format("{}", number);
    // Keep a number a number: 2.0 must not come back as the integer 2.
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    out += text;
}

bool write(std::string &out, const Value &value, int depth, std::string &error)
{
    if (depth > maximum_depth)
    {
        error = "value nesting is deeper than 64 levels";
        return false;
    }
    switch (value.type())
    {
    case ValueType::nil:
    case ValueType::any:
        out += "null";
        return true;
    case ValueType::boolean:
        out += value.as_bool() ? "true" : "false";
        return true;
    case ValueType::integer:
        out += std::to_string(value.as_integer());
        return true;
    case ValueType::number: {
        const double number = value.as_number();
        if (!std::isfinite(number))
        {
            error = "infinite and NaN numbers cannot be serialized";
            return false;
        }
        write_number(out, number);
        return true;
    }
    case ValueType::string:
        write_string(out, value.as_string());
        return true;
    case ValueType::vec3: {
        const Vec3 vector = value.as_vec3();
        out += "{\"$vector\":[";
        write_number(out, vector.x);
        out += ',';
        write_number(out, vector.y);
        out += ',';
        write_number(out, vector.z);
        out += "]}";
        return true;
    }
    case ValueType::handle:
        out += std::format("{{\"$handle\":[{},{}]}}",
                           static_cast<std::uint32_t>(value.as_handle().kind),
                           value.as_handle().id);
        return true;
    case ValueType::callback:
        error = "functions cannot be serialized";
        return false;
    case ValueType::array: {
        out += '[';
        bool first = true;
        for (const Value &element : value.as_array())
        {
            if (!first)
                out += ',';
            first = false;
            if (!write(out, element, depth + 1, error))
                return false;
        }
        out += ']';
        return true;
    }
    case ValueType::map: {
        out += '{';
        bool first = true;
        for (const auto &[key, element] : value.as_map())
        {
            if (!first)
                out += ',';
            first = false;
            write_string(out, key.starts_with('$') ? "$" + key : key);
            out += ':';
            if (!write(out, element, depth + 1, error))
                return false;
        }
        out += '}';
        return true;
    }
    }
    return true;
}

class Reader final
{
  public:
    explicit Reader(std::string_view text) : text_{text} {}

    bool document(Value &value, std::string &error)
    {
        if (!read(value, 0))
        {
            error = error_.empty() ? "malformed text" : error_;
            error += std::format(" (at byte {})", position_);
            return false;
        }
        skip();
        if (position_ != text_.size())
        {
            error = std::format("unexpected text after the value (at byte {})", position_);
            return false;
        }
        return true;
    }

  private:
    void skip() noexcept
    {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\n' || text_[position_] == '\r' ||
                text_[position_] == '\t'))
            ++position_;
    }

    bool fail(const char *message)
    {
        if (error_.empty())
            error_ = message;
        return false;
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(position_, word.size()) != word)
            return fail("unknown word");
        position_ += word.size();
        return true;
    }

    bool read(Value &value, int depth)
    {
        if (depth > maximum_depth)
            return fail("nesting is deeper than 64 levels");
        skip();
        if (position_ >= text_.size())
            return fail("unexpected end of text");
        const char next = text_[position_];
        if (next == 'n')
        {
            value = Value{};
            return literal("null");
        }
        if (next == 't')
        {
            value = true;
            return literal("true");
        }
        if (next == 'f')
        {
            value = false;
            return literal("false");
        }
        if (next == '"')
        {
            std::string text;
            if (!string(text))
                return false;
            value = std::move(text);
            return true;
        }
        if (next == '[')
            return array(value, depth);
        if (next == '{')
            return object(value, depth);
        return number(value);
    }

    bool number(Value &value)
    {
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-')
            ++position_;
        bool fractional{};
        while (position_ < text_.size())
        {
            const char character = text_[position_];
            if (character == '.' || character == 'e' || character == 'E' ||
                ((character == '+' || character == '-') && fractional))
                fractional = true;
            else if (character < '0' || character > '9')
                break;
            ++position_;
        }
        const std::string_view digits = text_.substr(start, position_ - start);
        if (digits.empty() || digits == "-")
            return fail("expected a value");
        if (!fractional)
        {
            std::int64_t integer{};
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), integer);
            if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size())
            {
                value = integer;
                return true;
            }
        }
        double number{};
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
            !std::isfinite(number))
            return fail("malformed number");
        value = number;
        return true;
    }

    static void append_utf8(std::string &out, std::uint32_t code)
    {
        if (code < 0x80U)
            out += static_cast<char>(code);
        else if (code < 0x800U)
        {
            out += static_cast<char>(0xC0U | (code >> 6U));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
        else if (code < 0x10000U)
        {
            out += static_cast<char>(0xE0U | (code >> 12U));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
        else
        {
            out += static_cast<char>(0xF0U | (code >> 18U));
            out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
    }

    bool hex4(std::uint32_t &code)
    {
        if (position_ + 4U > text_.size())
            return fail("short \\u escape");
        const auto parsed =
            std::from_chars(text_.data() + position_, text_.data() + position_ + 4U, code, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + position_ + 4U)
            return fail("malformed \\u escape");
        position_ += 4U;
        return true;
    }

    bool string(std::string &out)
    {
        ++position_; // opening quote
        while (position_ < text_.size())
        {
            const char character = text_[position_++];
            if (character == '"')
                return true;
            if (static_cast<unsigned char>(character) < 0x20U)
                return fail("control character in text");
            if (character != '\\')
            {
                out += character;
                continue;
            }
            if (position_ >= text_.size())
                break;
            const char escape = text_[position_++];
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                out += escape;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t code{};
                if (!hex4(code))
                    return false;
                if (code >= 0xD800U && code < 0xDC00U && text_.substr(position_, 2U) == "\\u")
                {
                    position_ += 2U;
                    std::uint32_t low{};
                    if (!hex4(low) || low < 0xDC00U || low >= 0xE000U)
                        return fail("broken surrogate pair");
                    code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
                }
                append_utf8(out, code);
                break;
            }
            default:
                return fail("unknown escape");
            }
        }
        return fail("unterminated text");
    }

    bool array(Value &value, int depth)
    {
        ++position_;
        Array array;
        skip();
        if (position_ < text_.size() && text_[position_] == ']')
        {
            ++position_;
            value = std::move(array);
            return true;
        }
        while (true)
        {
            Value element;
            if (!read(element, depth + 1))
                return false;
            array.push_back(std::move(element));
            skip();
            if (position_ >= text_.size())
                return fail("unterminated array");
            const char next = text_[position_++];
            if (next == ']')
                break;
            if (next != ',')
                return fail("expected , or ]");
        }
        value = std::move(array);
        return true;
    }

    bool object(Value &value, int depth)
    {
        ++position_;
        Map map;
        skip();
        if (position_ < text_.size() && text_[position_] == '}')
        {
            ++position_;
            value = std::move(map);
            return true;
        }
        while (true)
        {
            skip();
            if (position_ >= text_.size() || text_[position_] != '"')
                return fail("expected a key");
            std::string key;
            if (!string(key))
                return false;
            skip();
            if (position_ >= text_.size() || text_[position_++] != ':')
                return fail("expected :");
            Value element;
            if (!read(element, depth + 1))
                return false;
            for (const auto &[existing, ignored] : map)
                if (existing == key)
                    return fail("duplicate key");
            map.emplace_back(std::move(key), std::move(element));
            skip();
            if (position_ >= text_.size())
                return fail("unterminated object");
            const char next = text_[position_++];
            if (next == '}')
                break;
            if (next != ',')
                return fail("expected , or }");
        }
        // Tagged objects.
        if (map.size() == 1U && map.front().second.type() == ValueType::array)
        {
            const Array &parts = map.front().second.as_array();
            if (map.front().first == "$vector" && parts.size() == 3U &&
                std::all_of(parts.begin(), parts.end(), [](const Value &part) { return part.is_number(); }))
            {
                value = Vec3{parts[0].as_number(), parts[1].as_number(), parts[2].as_number()};
                return true;
            }
            if (map.front().first == "$handle" && parts.size() == 2U &&
                parts[0].type() == ValueType::integer && parts[1].type() == ValueType::integer)
            {
                value = Handle{static_cast<HandleKind>(parts[0].as_integer()),
                               static_cast<std::uint64_t>(parts[1].as_integer())};
                return true;
            }
        }
        for (auto &[key, element] : map)
            if (key.starts_with("$$"))
                key.erase(0U, 1U);
        value = std::move(map);
        return true;
    }

    std::string_view text_;
    std::size_t position_{};
    std::string error_;
};

} // namespace

bool serialize_value(const Value &value, std::string &text, std::string &error)
{
    std::string out;
    if (!write(out, value, 0, error))
        return false;
    text = std::move(out);
    error.clear();
    return true;
}

bool deserialize_value(std::string_view text, Value &value, std::string &error)
{
    Value parsed;
    if (!Reader{text}.document(parsed, error))
        return false;
    value = std::move(parsed);
    error.clear();
    return true;
}

} // namespace openminerz::modding
