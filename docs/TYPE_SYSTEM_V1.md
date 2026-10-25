# Cito Type System v1 — Direction

## Goal

Keep the normal application path strongly typed and trivial while allowing simulation, gateways, inspectors, and expert tooling to construct and manipulate the same message types dynamically.

## One canonical type system

The following are frontends to the same type identity and schema model:

- OMG IDL subset (primary schema source)
- generated C++ types (normal application path)
- `TypeBuilder` (optional advanced API)
- `DynamicData` (optional simulation/tooling API)

They must converge on the same `TypeId`, stable `FieldId`s, schema shape, and eventual wire representation.

## Public layering

Normal applications should only need:

```cpp
#include <cito/cito.hpp>

cito::Context ctx;
ctx.on<Position>(handler);
ctx.publish(Position{...});
```

Advanced users opt in explicitly.

Scalar shorthand remains small:

```cpp
auto position = cito::TypeBuilder("acme.navigation.Position")
    .member<double>(1, "x")
    .member<double>(2, "y")
    .member<std::string>(3, "frame").bound(16)
    .build();
```

Complex type expressions are explicit so nested bounds are unambiguous:

```cpp
auto telemetry = cito::TypeBuilder("acme.Telemetry")
    .member(1, "pose", cito::types::structure(position))
    .member(2, "coefficients",
        cito::types::array(cito::types::scalar<float>(), 4))
    .member(3, "tags",
        cito::types::sequence(cito::types::string(64), 20))
    .build();
```

The advanced API must not add runtime or conceptual cost to users that do not include or use it.

## Canonical model now represented

The prototype canonical model can describe:

- primitive scalar values
- bounded/unbounded strings
- enums
- nested structs
- fixed arrays
- bounded/unbounded sequences
- nested container element types
- optional fields
- stable explicit FieldIds

Nested struct/enum references carry their logical TypeId and SchemaHash so a parent SchemaHash changes when a referenced schema changes.

## Current prototype rules

- Type logical identity is based on canonical type name.
- Schema identity changes when fields or relevant field properties change.
- Field IDs are explicit, non-zero, and unique within a struct.
- Field names are metadata/tooling names, not intended to be transmitted on every data message.
- Container syntax is explicit: `array(element, extent)`, `sequence(element, bound)`, and `string(bound)`.
- The final TypeId/SchemaHash algorithm and width are not frozen.
- DynamicData now represents scalar/string/enum values plus nested structs and recursive array/sequence values. Bounds and fixed extents are validated before values enter the wire path.

## IDL/static status

The first IDL compiler slice is now implemented. The supported subset is documented in `IDL_V1.md`.

Generated types reconstruct the same canonical TypeId/SchemaHash as the equivalent manual TypeBuilder definition. Scalar, enum, nested struct, fixed array, sequence, and optional values now round-trip through DynamicData and the tagged wire codec.

## Next type work

1. strengthen nested schema-evolution tests
2. benchmark tagged complex encoding and identify measured fast-path opportunities
3. add unknown-field preservation only if gateways/tooling require it
4. add IDL typedef or additional constructs only when a real use case requires them

## Still deferred until a concrete need

- DDS-style keys/instances
- arbitrary annotations
- full XTypes assignability
- final hash contract
- compact/delimited fast representations
