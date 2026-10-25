# Cito Wire v1 — Prototype Direction

This document describes the current prototype used to validate schema evolution. It is not a frozen wire specification.

## User-visible rule

Schema evolution should work by default without transmitting field names on every message.

## Prototype tagged representation

Envelope:

```text
magic/version
TypeId
SchemaHash
field-count
```

Each present field:

```text
FieldId
TypeKind
payload-length
payload
```

Field names remain schema/tooling metadata and are not included in the data record.

## Evolution behavior currently tested

- same schema: round-trip
- newer sender -> older receiver: unknown FieldId is skipped
- older sender -> newer receiver: newly added optional field may be absent
- same FieldId with incompatible kind: rejected
- different TypeId: rejected
- truncated/corrupt payload: rejected
- field rename with stable FieldId/kind remains readable at the wire level

## Important non-final details

- TypeId and SchemaHash algorithms are prototype-only and are not yet cross-platform wire contracts.
- Scalar, string, enum, nested struct, fixed array, and sequence DynamicData values are encoded today.
- Required/optional semantics are intentionally minimal.
- IDL-generated static types now have a direct generated codec and do not require DynamicData on the normal encode/decode hot path.
- DynamicData remains the tooling/simulation/reference codec, and regression tests require direct-static output to be byte-for-byte identical to it.
- Nested structs recursively use the same Cito tagged envelope, so their fields keep the same FieldId-based evolution rules.
- Array/sequence payloads currently use count + per-element length + element payload framing. This is a correctness/evolution baseline, not a frozen performance format.
- Unknown fields are skipped during compatible decode; preservation/re-emission of unknown fields is not implemented yet.
- Nested/container decoding uses non-owning byte spans internally, and the transport packet layer exposes a zero-copy payload view.
- Compact or delimited fast paths are deferred until this tagged representation is benchmarked and the type model is stable.

The intended long-term rule remains: simple/evolvable by default, with faster representations added only when they provide measured value and without changing the application API.
