from __future__ import annotations

import importlib
import pathlib
import subprocess
import sys


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_idl_python_wire.py FIXTURE GENERATED_DIR")

    fixture = pathlib.Path(sys.argv[1])
    generated = pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(generated))

    position = importlib.import_module(
        "position")

    cpp_hex = subprocess.check_output(
        [str(fixture), "encode"],
        text=True,
    ).strip()

    py_value = position.Position(
        x=1.25,
        y=-2.5,
        frame="map",
        timestamp=123456,
        frame_kind=position.FrameKind.Map,
    )

    assert py_value.TYPE_ID == 3445279360899966310
    assert py_value.SCHEMA_HASH == 5316556461892417623
    assert py_value.encode().hex() == cpp_hex

    roundtrip = position.Position.decode(
        bytes.fromhex(cpp_hex))
    assert roundtrip == py_value

    python_only = position.Position(
        x=3.5,
        y=-7.25,
        frame="odom",
        frame_kind=position.FrameKind.Odom,
    )

    decoded = subprocess.check_output(
        [
            str(fixture),
            "decode",
            python_only.encode().hex(),
        ],
        text=True,
    ).strip()

    assert decoded == "ok"


if __name__ == "__main__":
    main()
