#pragma once

#include <cito/dynamic.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cito::wire {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Header {
    std::uint64_t type_id{};
    std::uint64_t schema_hash{};
    std::uint32_t field_count{};
};

namespace detail {

using Bytes = std::span<const std::uint8_t>;

inline void write_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

inline void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu));
    }
}

inline void write_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu));
    }
}

inline void patch_u32(
    std::vector<std::uint8_t>& out,
    std::size_t pos,
    std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw Error("Cito wire: payload too large");
    }

    const auto v = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        out[pos + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu);
    }
}

inline void require_range(
    Bytes bytes,
    std::size_t pos,
    std::size_t size,
    const char* message) {
    if (pos > bytes.size() || size > bytes.size() - pos) {
        throw Error(message);
    }
}

inline std::uint8_t read_u8(Bytes bytes, std::size_t& pos) {
    require_range(bytes, pos, 1, "Cito wire: truncated u8");
    return bytes[pos++];
}

inline std::uint32_t read_u32(Bytes bytes, std::size_t& pos) {
    require_range(bytes, pos, 4, "Cito wire: truncated u32");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[pos++]) << (i * 8);
    }
    return value;
}

inline std::uint64_t read_u64(Bytes bytes, std::size_t& pos) {
    require_range(bytes, pos, 8, "Cito wire: truncated u64");
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[pos++]) << (i * 8);
    }
    return value;
}

inline const Field* find_by_id(const Type& type, std::uint32_t id) noexcept {
    for (const auto& field : type.fields()) {
        if (field.id == id) return &field;
    }
    return nullptr;
}

inline void encode_message_into(
    std::vector<std::uint8_t>& out,
    const DynamicData& data);

inline void encode_value_into(
    std::vector<std::uint8_t>& out,
    const TypeSpec& spec,
    const DynamicValue& value) {
    DynamicData::validate(spec, value);

    switch (spec.kind) {
        case TypeKind::Bool:
            write_u8(out, std::get<bool>(value) ? 1u : 0u);
            return;
        case TypeKind::Int32:
        case TypeKind::Enum:
            write_u32(
                out,
                static_cast<std::uint32_t>(
                    std::get<std::int32_t>(value)));
            return;
        case TypeKind::UInt32:
            write_u32(out, std::get<std::uint32_t>(value));
            return;
        case TypeKind::Int64:
            write_u64(
                out,
                static_cast<std::uint64_t>(
                    std::get<std::int64_t>(value)));
            return;
        case TypeKind::UInt64:
            write_u64(out, std::get<std::uint64_t>(value));
            return;
        case TypeKind::Float32:
            write_u32(
                out,
                std::bit_cast<std::uint32_t>(
                    std::get<float>(value)));
            return;
        case TypeKind::Float64:
            write_u64(
                out,
                std::bit_cast<std::uint64_t>(
                    std::get<double>(value)));
            return;
        case TypeKind::String: {
            const auto& text = std::get<std::string>(value);
            out.insert(out.end(), text.begin(), text.end());
            return;
        }
        case TypeKind::Struct:
            encode_message_into(
                out,
                *std::get<DynamicStruct>(value));
            return;
        case TypeKind::Array:
        case TypeKind::Sequence: {
            const auto& list = std::get<DynamicListPtr>(value);
            if (list->values.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                throw Error("Cito wire: container too large");
            }

            write_u32(
                out,
                static_cast<std::uint32_t>(list->values.size()));

            for (const auto& item : list->values) {
                const auto length_pos = out.size();
                write_u32(out, 0);
                const auto payload_start = out.size();

                encode_value_into(out, *spec.element, item);

                patch_u32(
                    out,
                    length_pos,
                    out.size() - payload_start);
            }
            return;
        }
    }

    throw Error("Cito wire: unsupported type kind");
}

inline void encode_message_into(
    std::vector<std::uint8_t>& out,
    const DynamicData& data) {
    out.insert(out.end(), {'C', 'T', 'O', '1'});
    write_u64(out, data.type().type_id());
    write_u64(out, data.type().schema_hash());

    std::uint32_t count = 0;
    for (std::size_t i = 0; i < data.type().fields().size(); ++i) {
        if (data.value_at(i)) ++count;
    }
    write_u32(out, count);

    for (std::size_t i = 0; i < data.type().fields().size(); ++i) {
        const auto& maybe = data.value_at(i);
        if (!maybe) continue;

        const auto& field = data.type().fields()[i];
        write_u32(out, field.id);
        write_u8(out, static_cast<std::uint8_t>(field.type.kind));

        const auto length_pos = out.size();
        write_u32(out, 0);
        const auto payload_start = out.size();

        encode_value_into(out, field.type, *maybe);

        patch_u32(
            out,
            length_pos,
            out.size() - payload_start);
    }
}

inline Header inspect_header(Bytes bytes) {
    if (bytes.size() < 24 ||
        bytes[0] != 'C' ||
        bytes[1] != 'T' ||
        bytes[2] != 'O' ||
        bytes[3] != '1') {
        throw Error("Cito wire: invalid header");
    }

    std::size_t pos = 4;
    Header header;
    header.type_id = read_u64(bytes, pos);
    header.schema_hash = read_u64(bytes, pos);
    header.field_count = read_u32(bytes, pos);
    return header;
}

inline DynamicData decode_message(
    const Type& target,
    Bytes bytes);

inline DynamicValue decode_value(
    const TypeSpec& spec,
    Bytes bytes,
    std::size_t pos,
    std::uint32_t len) {
    require_range(
        bytes,
        pos,
        len,
        "Cito wire: truncated payload");

    auto cursor = pos;
    const auto expect = [len](std::uint32_t expected) {
        if (len != expected) {
            throw Error("Cito wire: invalid fixed-width field length");
        }
    };

    switch (spec.kind) {
        case TypeKind::Bool:
            expect(1);
            return read_u8(bytes, cursor) != 0;
        case TypeKind::Int32:
        case TypeKind::Enum:
            expect(4);
            return static_cast<std::int32_t>(
                read_u32(bytes, cursor));
        case TypeKind::UInt32:
            expect(4);
            return read_u32(bytes, cursor);
        case TypeKind::Int64:
            expect(8);
            return static_cast<std::int64_t>(
                read_u64(bytes, cursor));
        case TypeKind::UInt64:
            expect(8);
            return read_u64(bytes, cursor);
        case TypeKind::Float32:
            expect(4);
            return std::bit_cast<float>(
                read_u32(bytes, cursor));
        case TypeKind::Float64:
            expect(8);
            return std::bit_cast<double>(
                read_u64(bytes, cursor));
        case TypeKind::String:
            return std::string(
                reinterpret_cast<const char*>(
                    bytes.data() + pos),
                len);
        case TypeKind::Struct: {
            if (!spec.referenced_type) {
                throw Error(
                    "Cito wire: nested struct schema unavailable");
            }

            return std::make_shared<DynamicData>(
                decode_message(
                    *spec.referenced_type,
                    bytes.subspan(pos, len)));
        }
        case TypeKind::Array:
        case TypeKind::Sequence: {
            if (!spec.element) {
                throw Error(
                    "Cito wire: container element schema unavailable");
            }

            const auto end = pos + len;
            auto list = std::make_shared<DynamicList>();
            const auto count = read_u32(bytes, cursor);

            if (spec.kind == TypeKind::Array &&
                count != spec.extent) {
                throw Error("Cito wire: array extent mismatch");
            }

            if (spec.kind == TypeKind::Sequence &&
                spec.bound != 0 &&
                count > spec.bound) {
                throw Error("Cito wire: sequence bound exceeded");
            }

            list->values.reserve(count);

            for (std::uint32_t i = 0; i < count; ++i) {
                const auto item_len = read_u32(bytes, cursor);

                if (cursor > end ||
                    item_len > end - cursor) {
                    throw Error(
                        "Cito wire: truncated container element");
                }

                list->values.push_back(
                    decode_value(
                        *spec.element,
                        bytes,
                        cursor,
                        item_len));
                cursor += item_len;
            }

            if (cursor != end) {
                throw Error(
                    "Cito wire: trailing container bytes");
            }

            DynamicValue result = list;
            DynamicData::validate(spec, result);
            return result;
        }
    }

    throw Error("Cito wire: unsupported type kind");
}

inline DynamicData decode_message(
    const Type& target,
    Bytes bytes) {
    const auto header = inspect_header(bytes);

    if (header.type_id != target.type_id()) {
        throw Error("Cito wire: TypeId mismatch");
    }

    DynamicData data(target);
    std::size_t pos = 24;

    for (std::uint32_t i = 0; i < header.field_count; ++i) {
        const auto field_id = read_u32(bytes, pos);
        const auto encoded_kind_raw = read_u8(bytes, pos);

        if (encoded_kind_raw >
            static_cast<std::uint8_t>(TypeKind::Sequence)) {
            throw Error("Cito wire: unknown field kind");
        }

        const auto encoded_kind =
            static_cast<TypeKind>(encoded_kind_raw);
        const auto len = read_u32(bytes, pos);

        require_range(
            bytes,
            pos,
            len,
            "Cito wire: truncated field payload");

        if (const auto* field =
                find_by_id(target, field_id)) {
            if (field->type.kind != encoded_kind) {
                throw Error(
                    "Cito wire: incompatible field kind");
            }

            data.set_value(
                field->name,
                decode_value(
                    field->type,
                    bytes,
                    pos,
                    len));
        }

        pos += len;
    }

    if (pos != bytes.size()) {
        throw Error("Cito wire: trailing bytes");
    }

    for (const auto& field : target.fields()) {
        if (!field.optional && !data.has(field.name)) {
            throw Error("Cito wire: required field missing");
        }
    }

    return data;
}

} // namespace detail

inline Header inspect_header(
    std::span<const std::uint8_t> bytes) {
    return detail::inspect_header(bytes);
}

inline Header inspect_header(
    const std::vector<std::uint8_t>& bytes) {
    return detail::inspect_header(bytes);
}

inline std::vector<std::uint8_t> encode(
    const DynamicData& data) {
    std::vector<std::uint8_t> out;
    out.reserve(128);
    detail::encode_message_into(out, data);
    return out;
}

inline DynamicData decode(
    const Type& target,
    std::span<const std::uint8_t> bytes) {
    return detail::decode_message(target, bytes);
}

inline DynamicData decode(
    const Type& target,
    const std::vector<std::uint8_t>& bytes) {
    return detail::decode_message(target, bytes);
}

} // namespace cito::wire
