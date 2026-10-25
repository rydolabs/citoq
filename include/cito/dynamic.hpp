#pragma once

#include <cito/type.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cito {

class DynamicData;
struct DynamicList;

using DynamicStruct = std::shared_ptr<DynamicData>;
using DynamicListPtr = std::shared_ptr<DynamicList>;

using DynamicValue = std::variant<
    bool,
    std::int32_t,
    std::uint32_t,
    std::int64_t,
    std::uint64_t,
    float,
    double,
    std::string,
    DynamicStruct,
    DynamicListPtr>;

struct DynamicList {
    std::vector<DynamicValue> values;
};

class DynamicData {
public:
    explicit DynamicData(Type type) : type_(std::move(type)), values_(type_.fields().size()) {}

    const Type& type() const noexcept { return type_; }

    template <class T>
    void set(const std::string& field_name, T&& value) {
        set_value(field_name, normalize(std::forward<T>(value)));
    }

    void set_value(const std::string& field_name, DynamicValue value) {
        const auto index = find_index(field_name);
        const auto& field = type_.fields()[index];
        validate(field.type, value);
        values_[index] = std::move(value);
    }

    template <class T>
    const T& get(const std::string& field_name) const {
        return std::get<T>(value(field_name));
    }

    const DynamicValue& value(const std::string& field_name) const {
        const auto index = find_index(field_name);
        if (!values_[index]) {
            throw std::logic_error("DynamicData field is not set");
        }
        return *values_[index];
    }

    bool has(const std::string& field_name) const {
        return values_[find_index(field_name)].has_value();
    }

    const std::optional<DynamicValue>& value_at(std::size_t index) const {
        return values_.at(index);
    }

    class FieldProxy {
    public:
        FieldProxy(DynamicData& owner, std::string name) : owner_(owner), name_(std::move(name)) {}

        template <class T>
        FieldProxy& operator=(T&& value) {
            owner_.set(name_, std::forward<T>(value));
            return *this;
        }

    private:
        DynamicData& owner_;
        std::string name_;
    };

    FieldProxy operator[](std::string field_name) {
        (void)find_index(field_name);
        return FieldProxy(*this, std::move(field_name));
    }

    static void validate(const TypeSpec& spec, const DynamicValue& value) {
        switch (spec.kind) {
            case TypeKind::Bool:
                if (!std::holds_alternative<bool>(value)) mismatch();
                return;
            case TypeKind::Int32:
            case TypeKind::Enum:
                if (!std::holds_alternative<std::int32_t>(value)) mismatch();
                return;
            case TypeKind::UInt32:
                if (!std::holds_alternative<std::uint32_t>(value)) mismatch();
                return;
            case TypeKind::Int64:
                if (!std::holds_alternative<std::int64_t>(value)) mismatch();
                return;
            case TypeKind::UInt64:
                if (!std::holds_alternative<std::uint64_t>(value)) mismatch();
                return;
            case TypeKind::Float32:
                if (!std::holds_alternative<float>(value)) mismatch();
                return;
            case TypeKind::Float64:
                if (!std::holds_alternative<double>(value)) mismatch();
                return;
            case TypeKind::String: {
                if (!std::holds_alternative<std::string>(value)) mismatch();
                const auto& text = std::get<std::string>(value);
                if (spec.bound != 0 && text.size() > spec.bound) {
                    throw std::length_error("DynamicData string exceeds Cito bound");
                }
                return;
            }
            case TypeKind::Struct: {
                if (!std::holds_alternative<DynamicStruct>(value)) mismatch();
                const auto& nested = std::get<DynamicStruct>(value);
                if (!nested) {
                    throw std::invalid_argument("DynamicData struct value is null");
                }
                if (spec.referenced_type_id != 0 &&
                    nested->type().type_id() != spec.referenced_type_id) {
                    throw std::invalid_argument(
                        "DynamicData struct TypeId does not match Cito field type");
                }
                return;
            }
            case TypeKind::Array:
            case TypeKind::Sequence: {
                if (!std::holds_alternative<DynamicListPtr>(value)) mismatch();
                const auto& list = std::get<DynamicListPtr>(value);
                if (!list) {
                    throw std::invalid_argument("DynamicData list value is null");
                }
                if (!spec.element) {
                    throw std::logic_error("Cito container type has no element type");
                }
                if (spec.kind == TypeKind::Array && list->values.size() != spec.extent) {
                    throw std::length_error(
                        "DynamicData array extent does not match Cito type");
                }
                if (spec.kind == TypeKind::Sequence &&
                    spec.bound != 0 &&
                    list->values.size() > spec.bound) {
                    throw std::length_error("DynamicData sequence exceeds Cito bound");
                }
                for (const auto& item : list->values) {
                    validate(*spec.element, item);
                }
                return;
            }
        }
        mismatch();
    }

private:
    [[noreturn]] static void mismatch() {
        throw std::invalid_argument(
            "DynamicData value type does not match Cito field type");
    }

    std::size_t find_index(const std::string& name) const {
        const auto& fields = type_.fields();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == name) {
                return i;
            }
        }
        throw std::out_of_range("Unknown Cito DynamicData field: " + name);
    }

    static DynamicValue normalize(const char* value) {
        return std::string(value);
    }

    static DynamicValue normalize(char* value) {
        return std::string(value);
    }

    template <class T>
    static DynamicValue normalize(T&& value) {
        using V = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<V, std::string>) {
            return std::forward<T>(value);
        } else {
            return DynamicValue{std::forward<T>(value)};
        }
    }

    Type type_;
    std::vector<std::optional<DynamicValue>> values_;
};

} // namespace cito
