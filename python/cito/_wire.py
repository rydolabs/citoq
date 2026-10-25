from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct
from typing import Any

FNV_BASIS = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1

BOOL = 0
INT32 = 1
UINT32 = 2
INT64 = 3
UINT64 = 4
FLOAT32 = 5
FLOAT64 = 6
STRING = 7
ENUM = 8
STRUCT = 9
ARRAY = 10
SEQUENCE = 11


def _fnv(hash_value: int, data: bytes) -> int:
    h = hash_value
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & MASK64
    return h


def _hash_string(hash_value: int, value: str) -> int:
    return _fnv(hash_value, value.encode("utf-8"))


def _hash_u8(hash_value: int, value: int) -> int:
    return _fnv(hash_value, struct.pack("<B", value & 0xFF))


def _hash_u32(hash_value: int, value: int) -> int:
    return _fnv(hash_value, struct.pack("<I", value & 0xFFFFFFFF))


def _hash_i32(hash_value: int, value: int) -> int:
    return _hash_u32(hash_value, value)


def _hash_u64(hash_value: int, value: int) -> int:
    return _fnv(hash_value, struct.pack("<Q", value & MASK64))


@dataclass(frozen=True)
class EnumSpec:
    name: str
    values: tuple[tuple[str, int], ...]
    type_id: int
    schema_hash: int


@dataclass(frozen=True)
class TypeSpec:
    kind: int
    bound: int = 0
    extent: int = 0
    referenced_name: str = ""
    referenced_type_id: int = 0
    referenced_schema_hash: int = 0
    element: "TypeSpec | None" = None
    enum_type: type[IntEnum] | None = None
    struct_type: type | None = None
    struct_spec: "StructSpec | None" = None


@dataclass(frozen=True)
class FieldSpec:
    field_id: int
    name: str
    type_spec: TypeSpec
    optional: bool = False


@dataclass(frozen=True)
class StructSpec:
    name: str
    fields: tuple[FieldSpec, ...]
    type_id: int
    schema_hash: int


def enum_spec(
    name: str,
    enum_type: type[IntEnum],
    values: tuple[tuple[str, int], ...],
) -> EnumSpec:
    type_id = _hash_string(FNV_BASIS, name)
    schema_hash = type_id

    for value_name, value in values:
        schema_hash = _hash_i32(schema_hash, value)
        schema_hash = _hash_string(schema_hash, value_name)

    return EnumSpec(
        name,
        values,
        type_id,
        schema_hash)


def scalar(kind: int) -> TypeSpec:
    return TypeSpec(kind)


def string(bound: int = 0) -> TypeSpec:
    return TypeSpec(STRING, bound=bound)


def enumeration(
    spec: EnumSpec,
    enum_type: type[IntEnum]) -> TypeSpec:
    return TypeSpec(
        ENUM,
        referenced_name=spec.name,
        referenced_type_id=spec.type_id,
        referenced_schema_hash=spec.schema_hash,
        enum_type=enum_type)


def structure(
    spec: StructSpec,
    struct_type: type) -> TypeSpec:
    return TypeSpec(
        STRUCT,
        referenced_name=spec.name,
        referenced_type_id=spec.type_id,
        referenced_schema_hash=spec.schema_hash,
        struct_type=struct_type,
        struct_spec=spec)


def array(
    element: TypeSpec,
    extent: int) -> TypeSpec:
    if extent <= 0:
        raise ValueError(
            "Cito array extent must be positive")
    return TypeSpec(
        ARRAY,
        extent=extent,
        element=element)


def sequence(
    element: TypeSpec,
    bound: int = 0) -> TypeSpec:
    return TypeSpec(
        SEQUENCE,
        bound=bound,
        element=element)


def _hash_type_spec(
    hash_value: int,
    spec: TypeSpec) -> int:
    h = _hash_u8(
        hash_value,
        spec.kind)
    h = _hash_u64(h, spec.bound)
    h = _hash_u64(h, spec.extent)
    h = _hash_u64(
        h,
        spec.referenced_type_id)
    h = _hash_u64(
        h,
        spec.referenced_schema_hash)
    h = _hash_string(
        h,
        spec.referenced_name)

    if spec.element is not None:
        h = _hash_type_spec(
            h,
            spec.element)

    return h


def struct_spec(
    name: str,
    fields: tuple[FieldSpec, ...],
) -> StructSpec:
    type_id = _hash_string(
        FNV_BASIS,
        name)
    schema_hash = type_id
    seen_ids: set[int] = set()
    seen_names: set[str] = set()

    for field in fields:
        if field.field_id == 0:
            raise ValueError(
                "Cito field id 0 is reserved")
        if (
            field.field_id in seen_ids or
            field.name in seen_names
        ):
            raise ValueError(
                "duplicate Cito field")

        seen_ids.add(field.field_id)
        seen_names.add(field.name)

        schema_hash = _hash_u32(
            schema_hash,
            field.field_id)
        schema_hash = _hash_string(
            schema_hash,
            field.name)
        schema_hash = _hash_type_spec(
            schema_hash,
            field.type_spec)
        schema_hash = _hash_u8(
            schema_hash,
            1 if field.optional else 0)

    return StructSpec(
        name,
        fields,
        type_id,
        schema_hash)


def _encode_value(
    spec: TypeSpec,
    value: Any) -> bytes:
    if spec.kind == BOOL:
        return (
            b"\x01"
            if bool(value)
            else b"\x00")

    if spec.kind in (INT32, ENUM):
        return struct.pack(
            "<i",
            int(value))

    if spec.kind == UINT32:
        return struct.pack(
            "<I",
            int(value))

    if spec.kind == INT64:
        return struct.pack(
            "<q",
            int(value))

    if spec.kind == UINT64:
        return struct.pack(
            "<Q",
            int(value))

    if spec.kind == FLOAT32:
        return struct.pack(
            "<f",
            float(value))

    if spec.kind == FLOAT64:
        return struct.pack(
            "<d",
            float(value))

    if spec.kind == STRING:
        raw = str(value).encode("utf-8")
        if (
            spec.bound and
            len(raw) > spec.bound
        ):
            raise ValueError(
                "Cito string exceeds bound")
        return raw

    if spec.kind == STRUCT:
        if spec.struct_spec is None:
            raise ValueError(
                "Cito nested struct schema unavailable")
        return encode_struct(
            spec.struct_spec,
            value)

    if spec.kind in (ARRAY, SEQUENCE):
        if spec.element is None:
            raise ValueError(
                "Cito container element schema unavailable")

        items = list(value)

        if (
            spec.kind == ARRAY and
            len(items) != spec.extent
        ):
            raise ValueError(
                "Cito array extent mismatch")

        if (
            spec.kind == SEQUENCE and
            spec.bound and
            len(items) > spec.bound
        ):
            raise ValueError(
                "Cito sequence bound exceeded")

        out = bytearray(
            struct.pack(
                "<I",
                len(items)))

        for item in items:
            payload = _encode_value(
                spec.element,
                item)
            out += struct.pack(
                "<I",
                len(payload))
            out += payload

        return bytes(out)

    raise ValueError(
        "unsupported Cito type kind")


def encode_struct(
    spec: StructSpec,
    value: Any) -> bytes:
    present: list[
        tuple[FieldSpec, Any]] = []

    for field in spec.fields:
        item = getattr(
            value,
            field.name)

        if (
            field.optional and
            item is None
        ):
            continue

        present.append(
            (field, item))

    out = bytearray(b"CTO1")
    out += struct.pack(
        "<Q",
        spec.type_id)
    out += struct.pack(
        "<Q",
        spec.schema_hash)
    out += struct.pack(
        "<I",
        len(present))

    for field, item in present:
        payload = _encode_value(
            field.type_spec,
            item)

        out += struct.pack(
            "<IBI",
            field.field_id,
            field.type_spec.kind,
            len(payload))
        out += payload

    return bytes(out)


def _decode_value(
    spec: TypeSpec,
    data: memoryview) -> Any:
    raw = data

    if spec.kind == BOOL:
        if len(raw) != 1:
            raise ValueError(
                "Cito bool size mismatch")
        return raw[0] != 0

    if spec.kind == INT32:
        if len(raw) != 4:
            raise ValueError(
                "Cito int32 size mismatch")
        return struct.unpack(
            "<i",
            raw)[0]

    if spec.kind == ENUM:
        if len(raw) != 4:
            raise ValueError(
                "Cito enum size mismatch")
        number = struct.unpack(
            "<i",
            raw)[0]
        return (
            spec.enum_type(number)
            if spec.enum_type is not None
            else number)

    if spec.kind == UINT32:
        if len(raw) != 4:
            raise ValueError(
                "Cito uint32 size mismatch")
        return struct.unpack(
            "<I",
            raw)[0]

    if spec.kind == INT64:
        if len(raw) != 8:
            raise ValueError(
                "Cito int64 size mismatch")
        return struct.unpack(
            "<q",
            raw)[0]

    if spec.kind == UINT64:
        if len(raw) != 8:
            raise ValueError(
                "Cito uint64 size mismatch")
        return struct.unpack(
            "<Q",
            raw)[0]

    if spec.kind == FLOAT32:
        if len(raw) != 4:
            raise ValueError(
                "Cito float32 size mismatch")
        return struct.unpack(
            "<f",
            raw)[0]

    if spec.kind == FLOAT64:
        if len(raw) != 8:
            raise ValueError(
                "Cito float64 size mismatch")
        return struct.unpack(
            "<d",
            raw)[0]

    if spec.kind == STRING:
        if (
            spec.bound and
            len(raw) > spec.bound
        ):
            raise ValueError(
                "Cito string exceeds bound")
        return bytes(raw).decode(
            "utf-8")

    if spec.kind == STRUCT:
        if (
            spec.struct_type is None or
            spec.struct_spec is None
        ):
            raise ValueError(
                "Cito nested struct schema unavailable")

        return decode_struct(
            spec.struct_type,
            spec.struct_spec,
            raw)

    if spec.kind in (ARRAY, SEQUENCE):
        if (
            spec.element is None or
            len(raw) < 4
        ):
            raise ValueError(
                "Cito invalid container")

        count = struct.unpack_from(
            "<I",
            raw,
            0)[0]

        if (
            spec.kind == ARRAY and
            count != spec.extent
        ):
            raise ValueError(
                "Cito array extent mismatch")

        if (
            spec.kind == SEQUENCE and
            spec.bound and
            count > spec.bound
        ):
            raise ValueError(
                "Cito sequence bound exceeded")

        pos = 4
        out = []

        for _ in range(count):
            if pos + 4 > len(raw):
                raise ValueError(
                    "Cito truncated container")

            size = struct.unpack_from(
                "<I",
                raw,
                pos)[0]
            pos += 4

            if pos + size > len(raw):
                raise ValueError(
                    "Cito truncated container element")

            out.append(
                _decode_value(
                    spec.element,
                    raw[pos:pos + size]))
            pos += size

        if pos != len(raw):
            raise ValueError(
                "Cito trailing container bytes")

        return out

    raise ValueError(
        "unsupported Cito type kind")


def decode_struct(
    struct_type: type,
    spec: StructSpec,
    data: bytes | bytearray | memoryview):
    raw = memoryview(data)

    if (
        len(raw) < 24 or
        bytes(raw[:4]) != b"CTO1"
    ):
        raise ValueError(
            "Cito invalid header")

    type_id, _schema_hash, field_count = (
        struct.unpack_from(
            "<QQI",
            raw,
            4))

    if type_id != spec.type_id:
        raise ValueError(
            "Cito TypeId mismatch")

    fields_by_id = {
        field.field_id: field
        for field in spec.fields
    }

    values: dict[str, Any] = {}
    seen: set[int] = set()
    pos = 24

    for _ in range(field_count):
        if pos + 9 > len(raw):
            raise ValueError(
                "Cito truncated field header")

        field_id, kind, size = (
            struct.unpack_from(
                "<IBI",
                raw,
                pos))
        pos += 9

        if pos + size > len(raw):
            raise ValueError(
                "Cito truncated field payload")

        field = fields_by_id.get(
            field_id)

        if field is not None:
            if field_id in seen:
                raise ValueError(
                    "Cito duplicate field")

            if kind != field.type_spec.kind:
                raise ValueError(
                    "Cito incompatible field kind")

            seen.add(field_id)
            values[field.name] = (
                _decode_value(
                    field.type_spec,
                    raw[pos:pos + size]))

        pos += size

    if pos != len(raw):
        raise ValueError(
            "Cito trailing bytes")

    for field in spec.fields:
        if (
            not field.optional and
            field.field_id not in seen
        ):
            raise ValueError(
                "Cito required field missing: "
                + field.name)

    return struct_type(**values)
