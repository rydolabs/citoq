from __future__ import annotations

import cito
import position


def main() -> None:
    received: list[
        position.Position] = []

    value = position.Position(
        x=10.5,
        y=-4.25,
        frame="map",
        timestamp=42,
        frame_kind=position.FrameKind.Map,
    )

    class CountingMessage:
        TYPE_ID = 777

        def __init__(self) -> None:
            self.encodes = 0

        def encode(self) -> bytes:
            self.encodes += 1
            return b"counted"

    with (
        cito.Context("acme") as subscriber,
        cito.Context("acme") as publisher,
        cito.Context("other") as isolated,
    ):
        # subscribe() is Context-owned: ignoring the returned handle must
        # keep demand and callback state alive.
        ignored_received: list[
            position.Position] = []
        subscriber.subscribe(
            position.Position,
            ignored_received.append,
            "ignored")

        assert publisher.has_demand(
            position.Position,
            "ignored")
        assert (
            publisher.publish(
                value,
                "ignored") ==
            1)
        assert ignored_received == [value]

        sub = subscriber.subscribe(
            position.Position,
            received.append,
            "drone_17")

        assert publisher.has_demand(
            position.Position,
            "drone_17")
        assert not isolated.has_demand(
            position.Position,
            "drone_17")
        assert not publisher.has_demand(
            position.Position,
            "drone_18")

        counting = CountingMessage()
        assert (
            publisher.publish(
                counting,
                "nobody") ==
            0)
        assert counting.encodes == 0

        assert (
            publisher.publish(
                value,
                "drone_18") ==
            0)
        assert received == []

        assert (
            publisher.publish(
                value,
                "drone_17") ==
            1)

        assert received == [value]
        assert isinstance(
            received[0],
            position.Position)

        sub.close()
        assert not publisher.has_demand(
            position.Position,
            "drone_17")

        # Legacy on() remains scoped/RAII-compatible.
        subscriber.on(
            position.Position,
            lambda _: None,
            "scoped")
        assert not publisher.has_demand(
            position.Position,
            "scoped")

        assert (
            publisher.publish(
                value,
                "drone_17") ==
            0)


if __name__ == "__main__":
    main()
