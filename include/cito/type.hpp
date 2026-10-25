#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cito {

class Type;

enum class TypeKind : std::uint8_t {
    Bool,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
    Enum,
    Struct,
    Array,
    Sequence,
};

struct TypeSpec {
    TypeKind kind{};
    std::size_t bound{0};
    std::size_t extent{0};
    std::uint64_t referenced_type_id{0};
    std::uint64_t referenced_schema_hash{0};
    std::string referenced_name;
    std::shared_ptr<const TypeSpec> element;
    std::shared_ptr<const Type> referenced_type;
};

struct Field {
    std::uint32_t id{};
    std::string name;
    TypeSpec type;
    bool optional{false};
};

struct EnumValue {
    std::int32_t value{};
    std::string name;
};

class EnumType {
public:
    const std::string& name() const noexcept { return name_; }
    std::uint64_t type_id() const noexcept { return type_id_; }
    std::uint64_t schema_hash() const noexcept { return schema_hash_; }
    const std::vector<EnumValue>& values() const noexcept { return values_; }

private:
    friend class EnumBuilder;
    std::string name_;
    std::uint64_t type_id_{};
    std::uint64_t schema_hash_{};
    std::vector<EnumValue> values_;
};

class Type {
public:
    const std::string& name() const noexcept { return name_; }
    std::uint64_t type_id() const noexcept { return type_id_; }
    std::uint64_t schema_hash() const noexcept { return schema_hash_; }
    const std::vector<Field>& fields() const noexcept { return fields_; }

    const Field* find(std::string_view name) const noexcept {
        for (const auto& field : fields_) {
            if (field.name == name) return &field;
        }
        return nullptr;
    }

private:
    friend class TypeBuilder;
    std::string name_;
    std::uint64_t type_id_{};
    std::uint64_t schema_hash_{};
    std::vector<Field> fields_;
};

namespace detail {
inline std::uint64_t fnv1a(std::uint64_t hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::uint64_t hash_string(std::uint64_t hash, const std::string& value) noexcept {
    return fnv1a(hash, value.data(), value.size());
}

inline std::uint64_t hash_u8(
    std::uint64_t hash,
    std::uint8_t value) noexcept {
    return fnv1a(hash, &value, 1);
}

inline std::uint64_t hash_u32(
    std::uint64_t hash,
    std::uint32_t value) noexcept {
    unsigned char bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<unsigned char>(
            (value >> (i * 8)) & 0xffu);
    }
    return fnv1a(hash, bytes, sizeof(bytes));
}

inline std::uint64_t hash_i32(
    std::uint64_t hash,
    std::int32_t value) noexcept {
    return hash_u32(
        hash,
        static_cast<std::uint32_t>(value));
}

inline std::uint64_t hash_u64(
    std::uint64_t hash,
    std::uint64_t value) noexcept {
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>(
            (value >> (i * 8)) & 0xffu);
    }
    return fnv1a(hash, bytes, sizeof(bytes));
}

inline std::uint64_t hash_type_spec(std::uint64_t hash, const TypeSpec& spec) noexcept {
    const auto kind = static_cast<std::uint8_t>(spec.kind);
    hash = hash_u8(hash, kind);
    hash = hash_u64(hash, static_cast<std::uint64_t>(spec.bound));
    hash = hash_u64(hash, static_cast<std::uint64_t>(spec.extent));
    hash = hash_u64(hash, spec.referenced_type_id);
    hash = hash_u64(hash, spec.referenced_schema_hash);
    hash = hash_string(hash, spec.referenced_name);
    if (spec.element) hash = hash_type_spec(hash, *spec.element);
    return hash;
}

template <class T> struct kind_of;
template <> struct kind_of<bool> { static constexpr TypeKind value = TypeKind::Bool; };
template <> struct kind_of<std::int32_t> { static constexpr TypeKind value = TypeKind::Int32; };
template <> struct kind_of<std::uint32_t> { static constexpr TypeKind value = TypeKind::UInt32; };
template <> struct kind_of<std::int64_t> { static constexpr TypeKind value = TypeKind::Int64; };
template <> struct kind_of<std::uint64_t> { static constexpr TypeKind value = TypeKind::UInt64; };
template <> struct kind_of<float> { static constexpr TypeKind value = TypeKind::Float32; };
template <> struct kind_of<double> { static constexpr TypeKind value = TypeKind::Float64; };
template <> struct kind_of<std::string> { static constexpr TypeKind value = TypeKind::String; };
template <class T> constexpr TypeKind kind_of_v = kind_of<std::remove_cv_t<std::remove_reference_t<T>>>::value;
} // namespace detail

namespace types {

template <class T>
TypeSpec scalar() {
    TypeSpec spec{};
    spec.kind = detail::kind_of_v<T>;
    return spec;
}

inline TypeSpec string(std::size_t max_length = 0) {
    TypeSpec spec{};
    spec.kind = TypeKind::String;
    spec.bound = max_length;
    return spec;
}

inline TypeSpec array(TypeSpec element, std::size_t extent) {
    if (extent == 0) throw std::invalid_argument("Cito array extent must be greater than zero");
    TypeSpec spec{};
    spec.kind = TypeKind::Array;
    spec.extent = extent;
    spec.element = std::make_shared<TypeSpec>(std::move(element));
    return spec;
}

inline TypeSpec sequence(TypeSpec element, std::size_t max_elements = 0) {
    TypeSpec spec{};
    spec.kind = TypeKind::Sequence;
    spec.bound = max_elements;
    spec.element = std::make_shared<TypeSpec>(std::move(element));
    return spec;
}

inline TypeSpec structure(const Type& type) {
    TypeSpec spec{};
    spec.kind = TypeKind::Struct;
    spec.referenced_type_id = type.type_id();
    spec.referenced_schema_hash = type.schema_hash();
    spec.referenced_name = type.name();
    spec.referenced_type = std::make_shared<Type>(type);
    return spec;
}

inline TypeSpec enumeration(const EnumType& type) {
    TypeSpec spec{};
    spec.kind = TypeKind::Enum;
    spec.referenced_type_id = type.type_id();
    spec.referenced_schema_hash = type.schema_hash();
    spec.referenced_name = type.name();
    return spec;
}

} // namespace types

class EnumBuilder {
public:
    explicit EnumBuilder(std::string name) : name_(std::move(name)) {
        if (name_.empty()) throw std::invalid_argument("Cito enum name must not be empty");
    }

    EnumBuilder& value(std::int32_t value, std::string name) {
        if (name.empty()) throw std::invalid_argument("Cito enum value name must not be empty");
        values_.push_back(EnumValue{value, std::move(name)});
        return *this;
    }

    EnumType build() const {
        std::unordered_set<std::int32_t> numbers;
        std::unordered_set<std::string> names;
        for (const auto& value : values_) {
            if (!numbers.insert(value.value).second) throw std::invalid_argument("Duplicate Cito enum numeric value");
            if (!names.insert(value.name).second) throw std::invalid_argument("Duplicate Cito enum value name");
        }

        EnumType type;
        type.name_ = name_;
        type.values_ = values_;
        constexpr std::uint64_t basis = 14695981039346656037ull;
        type.type_id_ = detail::hash_string(basis, type.name_);
        auto schema = type.type_id_;
        for (const auto& value : type.values_) {
            schema = detail::hash_i32(schema, value.value);
            schema = detail::hash_string(schema, value.name);
        }
        type.schema_hash_ = schema;
        return type;
    }

private:
    std::string name_;
    std::vector<EnumValue> values_;
};

class TypeBuilder {
public:
    explicit TypeBuilder(std::string name) : name_(std::move(name)) {
        if (name_.empty()) throw std::invalid_argument("Cito type name must not be empty");
    }

    template <class T>
    TypeBuilder& member(std::uint32_t id, std::string name) {
        return member(id, std::move(name), types::scalar<T>());
    }

    TypeBuilder& member(std::uint32_t id, std::string name, TypeSpec type) {
        if (id == 0) throw std::invalid_argument("Cito field id 0 is reserved");
        if (name.empty()) throw std::invalid_argument("Cito field name must not be empty");
        fields_.push_back(Field{id, std::move(name), std::move(type), false});
        return *this;
    }

    TypeBuilder& optional() {
        last().optional = true;
        return *this;
    }

    TypeBuilder& bound(std::size_t max_length) {
        if (max_length == 0) throw std::invalid_argument("Cito bound must be greater than zero");
        auto& field = last();
        if (field.type.kind != TypeKind::String) {
            throw std::logic_error("Cito bound() shorthand applies only to a direct string field");
        }
        field.type.bound = max_length;
        return *this;
    }

    Type build() const {
        std::unordered_set<std::uint32_t> ids;
        std::unordered_set<std::string> names;
        for (const auto& field : fields_) {
            if (!ids.insert(field.id).second) throw std::invalid_argument("Duplicate Cito field id");
            if (!names.insert(field.name).second) throw std::invalid_argument("Duplicate Cito field name");
        }

        Type type;
        type.name_ = name_;
        type.fields_ = fields_;

        constexpr std::uint64_t basis = 14695981039346656037ull;
        type.type_id_ = detail::hash_string(basis, type.name_);

        auto schema = type.type_id_;
        for (const auto& field : type.fields_) {
            schema = detail::hash_u32(schema, field.id);
            schema = detail::hash_string(schema, field.name);
            schema = detail::hash_type_spec(schema, field.type);
            schema = detail::hash_u8(
                schema,
                field.optional ? 1u : 0u);
        }
        type.schema_hash_ = schema;
        return type;
    }

private:
    Field& last() {
        if (fields_.empty()) throw std::logic_error("Cito field modifier requires a preceding member()");
        return fields_.back();
    }

    std::string name_;
    std::vector<Field> fields_;
};

} // namespace cito
