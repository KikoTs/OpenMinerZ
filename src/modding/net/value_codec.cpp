// The mod channel's Value encoding and payload schemas (openminerz/modding/net.hpp).

#include "openminerz/modding/net.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <unordered_set>

namespace openminerz::modding
{
namespace
{

enum class Tag : std::uint8_t
{
    nil = 0,
    false_value = 1,
    true_value = 2,
    integer = 3,
    float32 = 4,
    float64 = 5,
    string = 6,
    vec3_32 = 7,
    vec3_64 = 8,
    handle = 9,
    array = 10,
    map = 11
};

void put_varint(std::vector<std::uint8_t> &out, std::uint64_t value)
{
    while (value >= 0x80U)
    {
        out.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_f32(std::vector<std::uint8_t> &out, float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        out.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFFU));
}

void put_f64(std::vector<std::uint8_t> &out, double value)
{
    const auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
        out.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFFU));
}

bool exact_f32(double value) noexcept
{
    return static_cast<double>(static_cast<float>(value)) == value;
}

class Encoder final
{
  public:
    Encoder(const ValueLimits &limits, std::vector<std::uint8_t> &out) : limits_{limits}, out_{out}
    {
    }

    bool encode(const Value &value, std::uint32_t depth)
    {
        if (depth > limits_.max_depth)
            return fail(std::format("payload nests deeper than {} levels", limits_.max_depth));
        if (++nodes_ > limits_.max_nodes)
            return fail(std::format("payload has more than {} values", limits_.max_nodes));
        const Value::Storage &storage = value.storage();
        switch (value.type())
        {
        case ValueType::nil:
            out_.push_back(static_cast<std::uint8_t>(Tag::nil));
            break;
        case ValueType::boolean:
            out_.push_back(static_cast<std::uint8_t>(std::get<bool>(storage) ? Tag::true_value
                                                                             : Tag::false_value));
            break;
        case ValueType::integer: {
            const std::int64_t integer = std::get<std::int64_t>(storage);
            out_.push_back(static_cast<std::uint8_t>(Tag::integer));
            put_varint(out_, (static_cast<std::uint64_t>(integer) << 1U) ^
                                 static_cast<std::uint64_t>(integer >> 63));
            break;
        }
        case ValueType::number: {
            const double number = std::get<double>(storage);
            if (!std::isfinite(number))
                return fail("payloads cannot contain NaN or infinite numbers");
            if (exact_f32(number))
            {
                out_.push_back(static_cast<std::uint8_t>(Tag::float32));
                put_f32(out_, static_cast<float>(number));
            }
            else
            {
                out_.push_back(static_cast<std::uint8_t>(Tag::float64));
                put_f64(out_, number);
            }
            break;
        }
        case ValueType::string: {
            const std::string &text = std::get<std::string>(storage);
            if (text.size() > limits_.max_string_bytes)
                return fail(
                    std::format("payload string exceeds {} bytes", limits_.max_string_bytes));
            out_.push_back(static_cast<std::uint8_t>(Tag::string));
            put_varint(out_, text.size());
            out_.insert(out_.end(), text.begin(), text.end());
            break;
        }
        case ValueType::vec3: {
            const Vec3 vector = std::get<Vec3>(storage);
            if (!std::isfinite(vector.x) || !std::isfinite(vector.y) || !std::isfinite(vector.z))
                return fail("payloads cannot contain NaN or infinite numbers");
            const bool narrow = exact_f32(vector.x) && exact_f32(vector.y) && exact_f32(vector.z);
            out_.push_back(static_cast<std::uint8_t>(narrow ? Tag::vec3_32 : Tag::vec3_64));
            for (const double component : {vector.x, vector.y, vector.z})
            {
                if (narrow)
                    put_f32(out_, static_cast<float>(component));
                else
                    put_f64(out_, component);
            }
            break;
        }
        case ValueType::handle: {
            const Handle handle = std::get<Handle>(storage);
            out_.push_back(static_cast<std::uint8_t>(Tag::handle));
            put_varint(out_, static_cast<std::uint32_t>(handle.kind));
            put_varint(out_, handle.id);
            break;
        }
        case ValueType::callback:
            return fail("payloads cannot contain functions");
        case ValueType::array: {
            const Array &array = value.as_array();
            out_.push_back(static_cast<std::uint8_t>(Tag::array));
            put_varint(out_, array.size());
            for (const Value &element : array)
                if (!encode(element, depth + 1U))
                    return false;
            break;
        }
        case ValueType::map: {
            const Map &map = value.as_map();
            out_.push_back(static_cast<std::uint8_t>(Tag::map));
            put_varint(out_, map.size());
            std::unordered_set<std::string_view> seen;
            for (const auto &[key, element] : map)
            {
                if (key.size() > limits_.max_key_bytes)
                    return fail(std::format("payload key exceeds {} bytes", limits_.max_key_bytes));
                if (!seen.insert(key).second)
                    return fail("payload map repeats key '" + key + "'");
                put_varint(out_, key.size());
                out_.insert(out_.end(), key.begin(), key.end());
                if (!encode(element, depth + 1U))
                    return false;
            }
            break;
        }
        case ValueType::any:
            return fail("invalid value");
        }
        if (out_.size() - start_ > limits_.max_encoded_bytes)
            return fail(std::format("payload exceeds {} bytes", limits_.max_encoded_bytes));
        return true;
    }

    std::string error;
    std::size_t start_{};

  private:
    bool fail(std::string message)
    {
        error = std::move(message);
        return false;
    }

    const ValueLimits &limits_;
    std::vector<std::uint8_t> &out_;
    std::uint32_t nodes_{};
};

class Decoder final
{
  public:
    Decoder(std::span<const std::uint8_t> bytes, const ValueLimits &limits)
        : bytes_{bytes}, limits_{limits}
    {
    }

    bool decode(Value &value, std::uint32_t depth)
    {
        if (depth > limits_.max_depth)
            return fail("payload nests too deeply");
        if (++nodes_ > limits_.max_nodes)
            return fail("payload has too many values");
        std::uint8_t tag{};
        if (!byte(tag))
            return false;
        switch (static_cast<Tag>(tag))
        {
        case Tag::nil:
            value = Value{};
            return true;
        case Tag::false_value:
            value = Value{false};
            return true;
        case Tag::true_value:
            value = Value{true};
            return true;
        case Tag::integer: {
            std::uint64_t zigzag{};
            if (!varint(zigzag))
                return false;
            value = Value{static_cast<std::int64_t>((zigzag >> 1U) ^ (0U - (zigzag & 1U)))};
            return true;
        }
        case Tag::float32:
        case Tag::float64: {
            double number{};
            if (!real(static_cast<Tag>(tag) == Tag::float32, number))
                return false;
            value = Value{number};
            return true;
        }
        case Tag::string: {
            std::string text;
            if (!string(text, limits_.max_string_bytes))
                return false;
            value = Value{std::move(text)};
            return true;
        }
        case Tag::vec3_32:
        case Tag::vec3_64: {
            const bool narrow = static_cast<Tag>(tag) == Tag::vec3_32;
            Vec3 vector;
            if (!real(narrow, vector.x) || !real(narrow, vector.y) || !real(narrow, vector.z))
                return false;
            value = Value{vector};
            return true;
        }
        case Tag::handle: {
            std::uint64_t kind{};
            Handle handle;
            if (!varint(kind) || !varint(handle.id))
                return false;
            if (kind > std::numeric_limits<std::uint32_t>::max())
                return fail("payload handle kind is out of range");
            handle.kind = static_cast<HandleKind>(kind);
            value = Value{handle};
            return true;
        }
        case Tag::array: {
            std::uint64_t count{};
            if (!varint(count))
                return false;
            // Every element takes at least one byte.
            if (count > remaining() || count > limits_.max_nodes)
                return fail("payload array count is out of range");
            Array array;
            array.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t index = 0U; index < count; ++index)
            {
                Value element;
                if (!decode(element, depth + 1U))
                    return false;
                array.push_back(std::move(element));
            }
            value = Value{std::move(array)};
            return true;
        }
        case Tag::map: {
            std::uint64_t count{};
            if (!varint(count))
                return false;
            // Every entry takes at least two bytes (key length, value tag).
            if (count > remaining() / 2U || count > limits_.max_nodes)
                return fail("payload map count is out of range");
            Map map;
            map.reserve(static_cast<std::size_t>(count));
            std::unordered_set<std::string> seen;
            for (std::uint64_t index = 0U; index < count; ++index)
            {
                std::string key;
                if (!string(key, limits_.max_key_bytes))
                    return false;
                if (!seen.insert(key).second)
                    return fail("payload map repeats a key");
                Value element;
                if (!decode(element, depth + 1U))
                    return false;
                map.emplace_back(std::move(key), std::move(element));
            }
            value = Value{std::move(map)};
            return true;
        }
        }
        return fail("payload has an unknown value tag");
    }

    [[nodiscard]] bool exhausted() const noexcept
    {
        return offset_ == bytes_.size();
    }
    [[nodiscard]] std::size_t consumed() const noexcept
    {
        return offset_;
    }

    std::string error;

  private:
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }

    bool fail(std::string message)
    {
        error = std::move(message);
        return false;
    }

    bool byte(std::uint8_t &value)
    {
        if (remaining() < 1U)
            return fail("payload is truncated");
        value = bytes_[offset_++];
        return true;
    }

    bool varint(std::uint64_t &value)
    {
        value = 0U;
        for (std::uint32_t shift = 0U; shift < 64U; shift += 7U)
        {
            std::uint8_t part{};
            if (!byte(part))
                return false;
            // The tenth byte may only carry the top bit.
            if (shift == 63U && (part & 0xFEU) != 0U)
                return fail("payload varint overflows");
            value |= static_cast<std::uint64_t>(part & 0x7FU) << shift;
            if ((part & 0x80U) == 0U)
            {
                // Reject padded encodings so every value has one form.
                if (part == 0U && shift != 0U)
                    return fail("payload varint is not minimal");
                return true;
            }
        }
        return fail("payload varint overflows");
    }

    bool real(bool narrow, double &value)
    {
        const std::size_t size = narrow ? 4U : 8U;
        if (remaining() < size)
            return fail("payload is truncated");
        std::uint64_t bits{};
        for (std::size_t index = 0U; index < size; ++index)
            bits |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U);
        offset_ += size;
        value = narrow ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits)))
                       : std::bit_cast<double>(bits);
        if (!std::isfinite(value))
            return fail("payload contains a non-finite number");
        return true;
    }

    bool string(std::string &text, std::uint32_t maximum)
    {
        std::uint64_t length{};
        if (!varint(length))
            return false;
        if (length > maximum || length > remaining())
            return fail("payload string length is out of range");
        text.assign(reinterpret_cast<const char *>(bytes_.data() + offset_),
                    static_cast<std::size_t>(length));
        offset_ += static_cast<std::size_t>(length);
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    const ValueLimits &limits_;
    std::size_t offset_{};
    std::uint32_t nodes_{};
};

bool parse_type_name(std::string_view text, ValueType &type, bool &optional)
{
    optional = !text.empty() && text.back() == '?';
    if (optional)
        text.remove_suffix(1U);
    static constexpr std::pair<std::string_view, ValueType> names[]{
        {"any", ValueType::any},         {"nil", ValueType::nil},
        {"boolean", ValueType::boolean}, {"integer", ValueType::integer},
        {"number", ValueType::number},   {"string", ValueType::string},
        {"vector", ValueType::vec3},     {"handle", ValueType::handle},
        {"array", ValueType::array},     {"table", ValueType::map}};
    for (const auto &[name, value] : names)
        if (text == name)
        {
            type = value;
            return true;
        }
    return false;
}

bool parse_fields(const Value &spec, PayloadSchema &schema, std::uint32_t depth,
                  std::string &error);

bool parse_field(const std::string &name, const Value &spec, FieldSchema &field,
                 std::uint32_t depth, std::string &error)
{
    field.name = name;
    if (spec.type() == ValueType::string)
    {
        if (!parse_type_name(spec.as_string(), field.type, field.optional))
        {
            error = "field '" + name + "' has unknown type '" + spec.as_string() + "'";
            return false;
        }
        return true;
    }
    if (spec.type() != ValueType::map)
    {
        error = "field '" + name + "' must be a type name or a table";
        return false;
    }
    const Value *type = spec.field("type");
    if (type == nullptr || type->type() != ValueType::string)
    {
        // A plain table of fields: a required nested table.
        field.type = ValueType::map;
        field.nested = std::make_shared<PayloadSchema>();
        return parse_fields(spec, *field.nested, depth + 1U, error);
    }
    if (!parse_type_name(type->as_string(), field.type, field.optional))
    {
        error = "field '" + name + "' has unknown type '" + type->as_string() + "'";
        return false;
    }
    for (const auto &[key, value] : spec.as_map())
    {
        if (key == "type")
            continue;
        if ((key == "min" || key == "max") && value.is_number())
            (key == "min" ? field.minimum : field.maximum) = value.as_number();
        else if (key == "optional" && value.type() == ValueType::boolean)
            field.optional = value.as_bool();
        else if (key == "fields" && value.type() == ValueType::map && field.type == ValueType::map)
        {
            field.nested = std::make_shared<PayloadSchema>();
            if (!parse_fields(value, *field.nested, depth + 1U, error))
                return false;
        }
        else
        {
            error = "field '" + name + "' has an unknown option '" + key + "'";
            return false;
        }
    }
    return true;
}

bool parse_fields(const Value &spec, PayloadSchema &schema, std::uint32_t depth, std::string &error)
{
    if (depth > 8U)
    {
        error = "schema nests too deeply";
        return false;
    }
    if (spec.type() != ValueType::map)
    {
        error = "a schema must be a table of fields";
        return false;
    }
    for (const auto &[name, value] : spec.as_map())
    {
        if (name == "_extra")
        {
            if (value.type() != ValueType::boolean)
            {
                error = "_extra must be a boolean";
                return false;
            }
            schema.allow_extra = value.as_bool();
            continue;
        }
        FieldSchema field;
        if (!parse_field(name, value, field, depth, error))
            return false;
        schema.fields.push_back(std::move(field));
    }
    return true;
}

bool type_matches(ValueType expected, const Value &value) noexcept
{
    switch (expected)
    {
    case ValueType::any:
        return true;
    case ValueType::number:
        return value.is_number();
    case ValueType::integer:
        if (value.type() == ValueType::integer)
            return true;
        if (value.type() == ValueType::number)
        {
            const double number = std::get<double>(value.storage());
            return std::trunc(number) == number && std::fabs(number) <= 9007199254740992.0;
        }
        return false;
    case ValueType::map:
        // An empty table arrives as an empty map; accept it as an empty array too.
        return value.type() == ValueType::map;
    case ValueType::array:
        return value.type() == ValueType::array ||
               (value.type() == ValueType::map && value.as_map().empty());
    default:
        return value.type() == expected;
    }
}

bool validate_map(const PayloadSchema &schema, const Value &payload, const std::string &path,
                  std::string &error)
{
    if (payload.type() != ValueType::map)
    {
        error = (path.empty() ? std::string{"payload"} : path) + " must be a table";
        return false;
    }
    for (const FieldSchema &field : schema.fields)
    {
        const std::string where = path.empty() ? field.name : path + "." + field.name;
        const Value *value = payload.field(field.name);
        if (value == nullptr || value->is_nil())
        {
            if (field.optional || field.type == ValueType::nil || field.type == ValueType::any)
                continue;
            error = "missing field " + where;
            return false;
        }
        if (!type_matches(field.type, *value))
        {
            error = std::string{"field "} + where + " must be " + value_type_name(field.type) +
                    ", got " + value_type_name(value->type());
            return false;
        }
        std::optional<double> measured;
        if (value->is_number())
            measured = value->as_number();
        else if (value->type() == ValueType::string)
            measured = static_cast<double>(value->as_string().size());
        else if (value->type() == ValueType::array)
            measured = static_cast<double>(value->as_array().size());
        if (measured.has_value() && ((field.minimum.has_value() && *measured < *field.minimum) ||
                                     (field.maximum.has_value() && *measured > *field.maximum)))
        {
            error = "field " + where + " is out of range";
            return false;
        }
        if (field.nested != nullptr && value->type() == ValueType::map &&
            !validate_map(*field.nested, *value, where, error))
            return false;
    }
    if (!schema.allow_extra)
        for (const auto &[name, value] : payload.as_map())
        {
            const bool known =
                std::any_of(schema.fields.begin(), schema.fields.end(),
                            [&](const FieldSchema &field) { return field.name == name; });
            if (!known)
            {
                error = "unexpected field " + (path.empty() ? name : path + "." + name);
                return false;
            }
        }
    return true;
}

} // namespace

bool encode_value(const Value &value, const ValueLimits &limits, std::vector<std::uint8_t> &out,
                  std::string &error)
{
    const std::size_t start = out.size();
    Encoder encoder{limits, out};
    encoder.start_ = start;
    if (!encoder.encode(value, 1U))
    {
        out.resize(start);
        error = std::move(encoder.error);
        return false;
    }
    error.clear();
    return true;
}

bool decode_value(std::span<const std::uint8_t> bytes, const ValueLimits &limits, Value &value,
                  std::string &error)
{
    if (bytes.size() > limits.max_encoded_bytes)
    {
        error = "payload is too large";
        return false;
    }
    Decoder decoder{bytes, limits};
    Value decoded;
    if (!decoder.decode(decoded, 1U))
    {
        error = std::move(decoder.error);
        return false;
    }
    if (!decoder.exhausted())
    {
        error = "payload has trailing bytes";
        return false;
    }
    value = std::move(decoded);
    error.clear();
    return true;
}

bool decode_value_prefix(std::span<const std::uint8_t> bytes, const ValueLimits &limits,
                         Value &value, std::size_t &consumed, std::string &error)
{
    Decoder decoder{bytes.first(std::min<std::size_t>(bytes.size(), limits.max_encoded_bytes)),
                    limits};
    Value decoded;
    if (!decoder.decode(decoded, 1U))
    {
        error = std::move(decoder.error);
        return false;
    }
    consumed = decoder.consumed();
    value = std::move(decoded);
    error.clear();
    return true;
}

bool parse_payload_schema(const Value &spec, PayloadSchema &schema, std::string &error)
{
    PayloadSchema parsed;
    if (!parse_fields(spec, parsed, 0U, error))
        return false;
    schema = std::move(parsed);
    error.clear();
    return true;
}

bool validate_payload(const PayloadSchema &schema, const Value &payload, std::string &error)
{
    // A schema with no fields still demands a table, so `nil` is refused.
    return validate_map(schema, payload, {}, error);
}

} // namespace openminerz::modding
