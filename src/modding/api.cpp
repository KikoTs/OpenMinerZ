#include "openminerz/modding/api.hpp"

#include <cmath>
#include <limits>

namespace openminerz::modding
{
namespace
{

[[noreturn]] void type_mismatch(ValueType expected, ValueType actual)
{
    throw ScriptError{std::string{"expected "} + value_type_name(expected) + ", got " +
                      value_type_name(actual)};
}

} // namespace

const char *value_type_name(ValueType type) noexcept
{
    switch (type)
    {
    case ValueType::nil:
        return "nil";
    case ValueType::boolean:
        return "boolean";
    case ValueType::integer:
        return "integer";
    case ValueType::number:
        return "number";
    case ValueType::string:
        return "string";
    case ValueType::vec3:
        return "vector";
    case ValueType::handle:
        return "handle";
    case ValueType::callback:
        return "function";
    case ValueType::array:
        return "array";
    case ValueType::map:
        return "table";
    case ValueType::any:
        return "any";
    }
    return "unknown";
}

bool Value::as_bool() const
{
    if (const bool *value = std::get_if<bool>(&storage_))
        return *value;
    type_mismatch(ValueType::boolean, type());
}

std::int64_t Value::as_integer() const
{
    if (const std::int64_t *value = std::get_if<std::int64_t>(&storage_))
        return *value;
    if (const double *value = std::get_if<double>(&storage_))
    {
        // Scripts only have doubles; accept whole numbers inside the exact range.
        constexpr double limit = 9007199254740992.0; // 2^53
        if (std::isfinite(*value) && std::trunc(*value) == *value && std::fabs(*value) <= limit)
            return static_cast<std::int64_t>(*value);
        throw ScriptError{"expected a whole number"};
    }
    type_mismatch(ValueType::integer, type());
}

double Value::as_number() const
{
    if (const double *value = std::get_if<double>(&storage_))
        return *value;
    if (const std::int64_t *value = std::get_if<std::int64_t>(&storage_))
        return static_cast<double>(*value);
    type_mismatch(ValueType::number, type());
}

const std::string &Value::as_string() const
{
    if (const std::string *value = std::get_if<std::string>(&storage_))
        return *value;
    type_mismatch(ValueType::string, type());
}

Vec3 Value::as_vec3() const
{
    if (const Vec3 *value = std::get_if<Vec3>(&storage_))
        return *value;
    type_mismatch(ValueType::vec3, type());
}

Handle Value::as_handle() const
{
    if (const Handle *value = std::get_if<Handle>(&storage_))
        return *value;
    type_mismatch(ValueType::handle, type());
}

Callback Value::as_callback() const
{
    if (const Callback *value = std::get_if<Callback>(&storage_))
        return *value;
    type_mismatch(ValueType::callback, type());
}

const Array &Value::as_array() const
{
    if (const auto *value = std::get_if<std::shared_ptr<Array>>(&storage_))
        return **value;
    // (Runtime) An empty Luau table arrives as an empty map; it is an empty
    // array just as well.
    if (const auto *map = std::get_if<std::shared_ptr<Map>>(&storage_); map && (*map)->empty())
    {
        static const Array empty;
        return empty;
    }
    type_mismatch(ValueType::array, type());
}

const Map &Value::as_map() const
{
    if (const auto *value = std::get_if<std::shared_ptr<Map>>(&storage_))
        return **value;
    if (const auto *array = std::get_if<std::shared_ptr<Array>>(&storage_);
        array && (*array)->empty())
    {
        static const Map empty;
        return empty;
    }
    type_mismatch(ValueType::map, type());
}

const Value *Value::field(std::string_view key) const noexcept
{
    const auto *map = std::get_if<std::shared_ptr<Map>>(&storage_);
    if (map == nullptr)
        return nullptr;
    for (const auto &[name, value] : **map)
    {
        if (name == key)
            return &value;
    }
    return nullptr;
}

Value Value::field_or(std::string_view key, Value fallback) const
{
    const Value *value = field(key);
    return value == nullptr || value->is_nil() ? std::move(fallback) : *value;
}

bool operator==(const Value &left, const Value &right)
{
    if (left.storage_.index() != right.storage_.index())
        return false;
    if (const auto *array = std::get_if<std::shared_ptr<Array>>(&left.storage_))
        return **array == *std::get<std::shared_ptr<Array>>(right.storage_);
    if (const auto *map = std::get_if<std::shared_ptr<Map>>(&left.storage_))
        return **map == *std::get<std::shared_ptr<Map>>(right.storage_);
    return left.storage_ == right.storage_;
}

} // namespace openminerz::modding
