#pragma once

#include <cito/wire.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cito {

template <class T>
struct StaticType;

template <class T>
struct StaticEnum;

template <class T>
struct StaticCodec;

template <class Request>
struct ReplyType;

template <class Request>
using ReplyTypeT =
    typename ReplyType<Request>::type;

template <class T>
const Type& static_type_ref() {
    static const Type type = StaticType<T>::type();
    return type;
}

template <class T>
Type type_of() {
    return static_type_ref<T>();
}

template <class T>
DynamicData to_dynamic(const T& value) {
    return StaticType<T>::to_dynamic(value);
}

template <class T>
T from_dynamic(const DynamicData& data) {
    return StaticType<T>::from_dynamic(data);
}

namespace detail {

template <class T>
struct is_std_array : std::false_type {};

template <class T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <class T>
struct is_std_vector : std::false_type {};

template <class T, class Allocator>
struct is_std_vector<std::vector<T, Allocator>> : std::true_type {};

template <class T>
inline constexpr bool scalar_dynamic_v =
    std::is_same_v<T, bool> ||
    std::is_same_v<T, std::int32_t> ||
    std::is_same_v<T, std::uint32_t> ||
    std::is_same_v<T, std::int64_t> ||
    std::is_same_v<T, std::uint64_t> ||
    std::is_same_v<T, float> ||
    std::is_same_v<T, double> ||
    std::is_same_v<T, std::string>;

} // namespace detail

template <class T>
DynamicValue to_dynamic_value(const T& value) {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;

    if constexpr (detail::scalar_dynamic_v<V>) {
        return DynamicValue{value};
    } else if constexpr (std::is_enum_v<V>) {
        return DynamicValue{static_cast<std::int32_t>(value)};
    } else if constexpr (
        detail::is_std_array<V>::value ||
        detail::is_std_vector<V>::value) {
        auto list = std::make_shared<DynamicList>();
        list->values.reserve(value.size());
        for (const auto& item : value) {
            list->values.push_back(to_dynamic_value(item));
        }
        return DynamicValue{std::move(list)};
    } else {
        return DynamicValue{
            std::make_shared<DynamicData>(to_dynamic(value))};
    }
}

template <class T>
T from_dynamic_value(const DynamicValue& value) {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;

    if constexpr (detail::scalar_dynamic_v<V>) {
        return std::get<V>(value);
    } else if constexpr (std::is_enum_v<V>) {
        return static_cast<V>(std::get<std::int32_t>(value));
    } else if constexpr (detail::is_std_array<V>::value) {
        const auto& list = std::get<DynamicListPtr>(value);
        if (!list) {
            throw std::invalid_argument("Cito dynamic array is null");
        }

        V out{};
        if (list->values.size() != out.size()) {
            throw std::length_error("Cito dynamic array size mismatch");
        }

        using Item = typename V::value_type;
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = from_dynamic_value<Item>(list->values[i]);
        }
        return out;
    } else if constexpr (detail::is_std_vector<V>::value) {
        const auto& list = std::get<DynamicListPtr>(value);
        if (!list) {
            throw std::invalid_argument("Cito dynamic sequence is null");
        }

        V out;
        out.reserve(list->values.size());
        using Item = typename V::value_type;
        for (const auto& item : list->values) {
            out.push_back(from_dynamic_value<Item>(item));
        }
        return out;
    } else {
        const auto& nested = std::get<DynamicStruct>(value);
        if (!nested) {
            throw std::invalid_argument("Cito dynamic struct is null");
        }
        return from_dynamic<V>(*nested);
    }
}

namespace static_detail {

inline void require_range(
    std::span<const std::uint8_t> bytes,
    std::size_t pos,
    std::size_t size,
    const char* message) {
    if (pos > bytes.size() || size > bytes.size() - pos) {
        throw wire::Error(message);
    }
}

inline void require_kind(
    const TypeSpec& spec,
    TypeKind expected) {
    if (spec.kind != expected) {
        throw wire::Error("Cito static codec: schema kind mismatch");
    }
}

template <class T>
void encode_value_into(
    std::vector<std::uint8_t>& out,
    const TypeSpec& spec,
    const T& value) {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;

    if constexpr (std::is_same_v<V, bool>) {
        require_kind(spec, TypeKind::Bool);
        wire::detail::write_u8(out, value ? 1u : 0u);
    } else if constexpr (std::is_same_v<V, std::int32_t>) {
        require_kind(spec, TypeKind::Int32);
        wire::detail::write_u32(out, static_cast<std::uint32_t>(value));
    } else if constexpr (std::is_same_v<V, std::uint32_t>) {
        require_kind(spec, TypeKind::UInt32);
        wire::detail::write_u32(out, value);
    } else if constexpr (std::is_same_v<V, std::int64_t>) {
        require_kind(spec, TypeKind::Int64);
        wire::detail::write_u64(out, static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_same_v<V, std::uint64_t>) {
        require_kind(spec, TypeKind::UInt64);
        wire::detail::write_u64(out, value);
    } else if constexpr (std::is_same_v<V, float>) {
        require_kind(spec, TypeKind::Float32);
        wire::detail::write_u32(out, std::bit_cast<std::uint32_t>(value));
    } else if constexpr (std::is_same_v<V, double>) {
        require_kind(spec, TypeKind::Float64);
        wire::detail::write_u64(out, std::bit_cast<std::uint64_t>(value));
    } else if constexpr (std::is_same_v<V, std::string>) {
        require_kind(spec, TypeKind::String);
        if (spec.bound != 0 && value.size() > spec.bound) {
            throw std::length_error("Cito static string exceeds bound");
        }
        out.insert(out.end(), value.begin(), value.end());
    } else if constexpr (std::is_enum_v<V>) {
        require_kind(spec, TypeKind::Enum);
        wire::detail::write_u32(
            out,
            static_cast<std::uint32_t>(
                static_cast<std::int32_t>(value)));
    } else if constexpr (detail::is_std_array<V>::value) {
        require_kind(spec, TypeKind::Array);
        if (!spec.element || value.size() != spec.extent) {
            throw std::length_error("Cito static array extent mismatch");
        }

        wire::detail::write_u32(
            out,
            static_cast<std::uint32_t>(value.size()));

        for (const auto& item : value) {
            const auto length_pos = out.size();
            wire::detail::write_u32(out, 0);
            const auto payload_start = out.size();
            encode_value_into(out, *spec.element, item);
            wire::detail::patch_u32(
                out,
                length_pos,
                out.size() - payload_start);
        }
    } else if constexpr (detail::is_std_vector<V>::value) {
        require_kind(spec, TypeKind::Sequence);
        if (!spec.element) {
            throw wire::Error("Cito static sequence element type missing");
        }
        if (spec.bound != 0 && value.size() > spec.bound) {
            throw std::length_error("Cito static sequence exceeds bound");
        }
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Cito static sequence too large");
        }

        wire::detail::write_u32(
            out,
            static_cast<std::uint32_t>(value.size()));

        for (const auto& item : value) {
            const auto length_pos = out.size();
            wire::detail::write_u32(out, 0);
            const auto payload_start = out.size();
            encode_value_into(out, *spec.element, item);
            wire::detail::patch_u32(
                out,
                length_pos,
                out.size() - payload_start);
        }
    } else {
        require_kind(spec, TypeKind::Struct);
        if (spec.referenced_type_id != 0 &&
            spec.referenced_type_id != static_type_ref<V>().type_id()) {
            throw wire::Error("Cito static nested TypeId mismatch");
        }
        StaticCodec<V>::encode_into(out, value);
    }
}

template <class T>
T decode_value(
    const TypeSpec& spec,
    std::span<const std::uint8_t> bytes) {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;
    std::size_t pos = 0;

    if constexpr (std::is_same_v<V, bool>) {
        require_kind(spec, TypeKind::Bool);
        if (bytes.size() != 1) throw wire::Error("Cito static bool size mismatch");
        return wire::detail::read_u8(bytes, pos) != 0;
    } else if constexpr (std::is_same_v<V, std::int32_t>) {
        require_kind(spec, TypeKind::Int32);
        if (bytes.size() != 4) throw wire::Error("Cito static int32 size mismatch");
        return static_cast<std::int32_t>(wire::detail::read_u32(bytes, pos));
    } else if constexpr (std::is_same_v<V, std::uint32_t>) {
        require_kind(spec, TypeKind::UInt32);
        if (bytes.size() != 4) throw wire::Error("Cito static uint32 size mismatch");
        return wire::detail::read_u32(bytes, pos);
    } else if constexpr (std::is_same_v<V, std::int64_t>) {
        require_kind(spec, TypeKind::Int64);
        if (bytes.size() != 8) throw wire::Error("Cito static int64 size mismatch");
        return static_cast<std::int64_t>(wire::detail::read_u64(bytes, pos));
    } else if constexpr (std::is_same_v<V, std::uint64_t>) {
        require_kind(spec, TypeKind::UInt64);
        if (bytes.size() != 8) throw wire::Error("Cito static uint64 size mismatch");
        return wire::detail::read_u64(bytes, pos);
    } else if constexpr (std::is_same_v<V, float>) {
        require_kind(spec, TypeKind::Float32);
        if (bytes.size() != 4) throw wire::Error("Cito static float size mismatch");
        return std::bit_cast<float>(wire::detail::read_u32(bytes, pos));
    } else if constexpr (std::is_same_v<V, double>) {
        require_kind(spec, TypeKind::Float64);
        if (bytes.size() != 8) throw wire::Error("Cito static double size mismatch");
        return std::bit_cast<double>(wire::detail::read_u64(bytes, pos));
    } else if constexpr (std::is_same_v<V, std::string>) {
        require_kind(spec, TypeKind::String);
        if (spec.bound != 0 && bytes.size() > spec.bound) {
            throw std::length_error("Cito static string exceeds bound");
        }
        return std::string(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size());
    } else if constexpr (std::is_enum_v<V>) {
        require_kind(spec, TypeKind::Enum);
        if (bytes.size() != 4) throw wire::Error("Cito static enum size mismatch");
        return static_cast<V>(
            static_cast<std::int32_t>(
                wire::detail::read_u32(bytes, pos)));
    } else if constexpr (detail::is_std_array<V>::value) {
        require_kind(spec, TypeKind::Array);
        if (!spec.element) {
            throw wire::Error("Cito static array element type missing");
        }

        const auto count = wire::detail::read_u32(bytes, pos);
        V out{};
        if (count != out.size() || count != spec.extent) {
            throw std::length_error("Cito static array extent mismatch");
        }

        using Item = typename V::value_type;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto len = wire::detail::read_u32(bytes, pos);
            require_range(
                bytes,
                pos,
                len,
                "Cito static codec: truncated array element");
            out[i] = decode_value<Item>(
                *spec.element,
                bytes.subspan(pos, len));
            pos += len;
        }

        if (pos != bytes.size()) {
            throw wire::Error("Cito static array trailing bytes");
        }
        return out;
    } else if constexpr (detail::is_std_vector<V>::value) {
        require_kind(spec, TypeKind::Sequence);
        if (!spec.element) {
            throw wire::Error("Cito static sequence element type missing");
        }

        const auto count = wire::detail::read_u32(bytes, pos);
        if (spec.bound != 0 && count > spec.bound) {
            throw std::length_error("Cito static sequence exceeds bound");
        }

        V out;
        out.reserve(count);
        using Item = typename V::value_type;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto len = wire::detail::read_u32(bytes, pos);
            require_range(
                bytes,
                pos,
                len,
                "Cito static codec: truncated sequence element");
            out.push_back(decode_value<Item>(
                *spec.element,
                bytes.subspan(pos, len)));
            pos += len;
        }

        if (pos != bytes.size()) {
            throw wire::Error("Cito static sequence trailing bytes");
        }
        return out;
    } else {
        require_kind(spec, TypeKind::Struct);
        return StaticCodec<V>::decode(bytes);
    }
}

inline void write_header(
    std::vector<std::uint8_t>& out,
    const Type& type,
    std::uint32_t field_count) {
    out.insert(out.end(), {'C', 'T', 'O', '1'});
    wire::detail::write_u64(out, type.type_id());
    wire::detail::write_u64(out, type.schema_hash());
    wire::detail::write_u32(out, field_count);
}

template <class T>
void encode_field(
    std::vector<std::uint8_t>& out,
    const Field& field,
    const T& value) {
    wire::detail::write_u32(out, field.id);
    wire::detail::write_u8(
        out,
        static_cast<std::uint8_t>(field.type.kind));

    const auto length_pos = out.size();
    wire::detail::write_u32(out, 0);
    const auto payload_start = out.size();

    encode_value_into(out, field.type, value);

    wire::detail::patch_u32(
        out,
        length_pos,
        out.size() - payload_start);
}

struct FieldView {
    std::uint32_t id{};
    TypeKind kind{};
    std::span<const std::uint8_t> payload;
};

inline wire::Header begin_decode(
    const Type& type,
    std::span<const std::uint8_t> bytes) {
    const auto header = wire::inspect_header(bytes);
    if (header.type_id != type.type_id()) {
        throw wire::Error("Cito static codec: TypeId mismatch");
    }
    return header;
}

inline FieldView read_field(
    std::span<const std::uint8_t> bytes,
    std::size_t& pos) {
    const auto id = wire::detail::read_u32(bytes, pos);
    const auto kind_raw = wire::detail::read_u8(bytes, pos);
    if (kind_raw > static_cast<std::uint8_t>(TypeKind::Sequence)) {
        throw wire::Error("Cito static codec: unknown field kind");
    }

    const auto len = wire::detail::read_u32(bytes, pos);
    require_range(
        bytes,
        pos,
        len,
        "Cito static codec: truncated field payload");

    FieldView field{
        id,
        static_cast<TypeKind>(kind_raw),
        bytes.subspan(pos, len)};
    pos += len;
    return field;
}

inline void accept_field(
    const FieldView& encoded,
    const Field& expected,
    bool& seen) {
    if (encoded.kind != expected.type.kind) {
        throw wire::Error("Cito static codec: incompatible field kind");
    }
    if (seen) {
        throw wire::Error("Cito static codec: duplicate field");
    }
    seen = true;
}

inline void finish_decode(
    std::span<const std::uint8_t> bytes,
    std::size_t pos) {
    if (pos != bytes.size()) {
        throw wire::Error("Cito static codec: trailing bytes");
    }
}

inline void require_present(
    bool seen,
    const char* field_name) {
    if (!seen) {
        throw wire::Error(
            std::string("Cito static codec: required field missing: ") +
            field_name);
    }
}

} // namespace static_detail

// Reference fallback for handwritten StaticType specializations. IDL-generated
// types specialize StaticCodec and bypass DynamicData on the production path.
template <class T>
struct StaticCodec {
    static constexpr bool direct = false;

    static std::vector<std::uint8_t> encode(const T& value) {
        return wire::encode(to_dynamic(value));
    }

    static T decode(std::span<const std::uint8_t> bytes) {
        return from_dynamic<T>(
            wire::decode(static_type_ref<T>(), bytes));
    }
};

template <class T>
std::vector<std::uint8_t> encode(const T& value) {
    return StaticCodec<T>::encode(value);
}

template <class T>
T decode(std::span<const std::uint8_t> bytes) {
    return StaticCodec<T>::decode(bytes);
}

template <class T>
T decode(const std::vector<std::uint8_t>& bytes) {
    return StaticCodec<T>::decode(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

} // namespace cito
