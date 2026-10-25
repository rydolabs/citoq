from __future__ import annotations

from enum import IntEnum

from cito import _wire


class FrameKind(IntEnum):
    Map = 0
    Odom = 1


def main() -> None:
    enum = _wire.enum_spec(
        "acme.navigation.FrameKind",
        FrameKind,
        (("Map", 0), ("Odom", 1)),
    )
    assert enum.type_id == 1114787075986681732
    assert enum.schema_hash == 2389695264774907256

    position = _wire.struct_spec(
        "acme.navigation.Position",
        (
            _wire.FieldSpec(
                1,
                "x",
                _wire.scalar(_wire.FLOAT64)),
            _wire.FieldSpec(
                2,
                "y",
                _wire.scalar(_wire.FLOAT64)),
            _wire.FieldSpec(
                3,
                "frame",
                _wire.string(16)),
            _wire.FieldSpec(
                4,
                "timestamp",
                _wire.scalar(_wire.UINT64),
                True),
            _wire.FieldSpec(
                5,
                "frame_kind",
                _wire.enumeration(
                    enum,
                    FrameKind),
                True),
        ),
    )

    assert position.type_id == 3445279360899966310
    assert position.schema_hash == 5316556461892417623


if __name__ == "__main__":
    main()
