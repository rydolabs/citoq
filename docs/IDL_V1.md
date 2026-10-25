# Cito IDL v1 — Supported Subset

## Purpose

Cito uses an intentionally small OMG IDL data-definition subset as the primary schema language. It is not a full CORBA IDL implementation and it does not expose DDS participants, topics, writers/readers, keys, QoS, or instance lifecycle.

The compiler entry point is:

```bash
python3 tools/cito_idlc.py schema.idl -o schema.hpp
```

Generated C++ types carry Cito static adapters that reconstruct the same canonical `Type` / `EnumType` model used by `TypeBuilder`.

## Supported syntax

Current prototype support:

- `module`, including nested modules
- `struct`
- `enum`, including explicit numeric enum values
- `boolean`
- `long` -> 32-bit signed
- `unsigned long` -> 32-bit unsigned
- `long long` -> 64-bit signed
- `unsigned long long` -> 64-bit unsigned
- `float`
- `double`
- `string` and `string<N>`
- `sequence<T>` and `sequence<T, N>`
- fixed arrays such as `float values[4]`
- user-defined struct/enum references
- `@id(N)`
- `@optional`
- line and block comments

Every struct member must have an explicit non-zero `@id(N)`. This is deliberate: Cito treats stable FieldIds as part of the schema-evolution contract and does not silently derive them from field order.

## Example

```idl
module acme {
  module navigation {
    enum FrameKind {
      Map = 0,
      Odom = 1
    };

    struct Position {
      @id(1) double x;
      @id(2) double y;
      @id(3) string<16> frame;
      @id(4) @optional unsigned long long timestamp;
      @id(5) @optional FrameKind frame_kind;
    };
  };
};
```

The normal application uses the generated type, not `TypeBuilder`:

```cpp
acme::navigation::Position p;
p.x = 1.0;
p.y = 2.0;

ctx.publish(p);
```

The generated metadata is equivalent to building the same canonical model manually. The smoke test checks equality of TypeId and SchemaHash between the generated `Position` and its manual `TypeBuilder` equivalent.

## Static / dynamic / wire interoperability

Generated types have two intentionally compatible paths:

```text
normal application hot path:

generated C++ value
      |
      v
generated StaticCodec<T>
      |
      v
tagged Cito wire


tooling / simulation / reference path:

generated C++ value
      |
      v
DynamicData
      |
      v
tagged Cito wire
```

The generated direct codec handles scalar, string, enum, optional, nested struct, fixed array, and sequence values without converting the production message through DynamicData.

Regression smoke tests require the direct static bytes to be byte-for-byte identical to the DynamicData reference bytes. This keeps one wire/type system while allowing the normal generated path to avoid reflection, field-name lookup, variant trees, and shared-pointer allocation.

## Complex types

Nested structs, arrays, and sequences now participate in generated static <-> DynamicData conversion and tagged wire round-trips. Container bounds and fixed extents are validated by the canonical TypeSpec before encoding.

## Not supported yet

- typedef
- union
- bitset / bitmask
- interfaces, operations, exceptions, valuetypes
- arbitrary annotations
- DDS key semantics
- full XTypes assignability/extensibility rules
- absolute leading scoped names
- constants and expressions beyond enum integer literals / positive bounds

These should only be added when they solve a concrete Cito use case without leaking complexity into the normal `publish/on` API.
