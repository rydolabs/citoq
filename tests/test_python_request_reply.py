from __future__ import annotations

import asyncio

import cito
import request_reply


class TinyReply:
    TYPE_ID = 902

    def __init__(self, value: int):
        self.value = value

    def encode(self) -> bytes:
        return bytes([self.value])

    @classmethod
    def decode(cls, data):
        raw = bytes(data)
        assert len(raw) == 1
        return cls(raw[0])


class TinyRequest:
    TYPE_ID = 901
    REPLY_TYPE = TinyReply
    encodes = 0

    def __init__(self, value: int):
        self.value = value

    def encode(self) -> bytes:
        type(self).encodes += 1
        return bytes([self.value])

    @classmethod
    def decode(cls, data):
        raw = bytes(data)
        assert len(raw) == 1
        return cls(raw[0])


async def main() -> None:
    with (
        cito.Context("acme") as provider,
        cito.Context("acme") as requester,
        cito.Context("other") as isolated,
    ):
        # handle() is Context-owned; ignoring the returned handle is valid.
        provider.handle(
            request_reply.GetStatus,
            lambda req: request_reply.Status(
                battery=84,
                armed=(
                    req.drone_id ==
                    "drone_17")),
            "drone_17",
        )

        assert requester.has_handler(
            request_reply.GetStatus,
            "drone_17")
        assert not isolated.has_handler(
            request_reply.GetStatus,
            "drone_17")

        status = await requester.request(
            request_reply.GetStatus(
                drone_id="drone_17"),
            "drone_17",
        )

        assert status == request_reply.Status(
            battery=84,
            armed=True)

        # No handler must fail before request serialization.
        TinyRequest.encodes = 0
        try:
            await requester.request(
                TinyRequest(7),
                "missing")
        except cito.Unavailable:
            pass
        else:
            raise AssertionError(
                "missing handler did not fail")

        assert TinyRequest.encodes == 0

        # Explicit close is available for dynamic registrations.
        temporary = provider.handle(
            TinyRequest,
            lambda req: TinyReply(
                req.value + 1),
            "temporary",
        )

        reply = await requester.request(
            TinyRequest(7),
            "temporary")
        assert reply.value == 8

        temporary.close()
        assert not requester.has_handler(
            TinyRequest,
            "temporary")


if __name__ == "__main__":
    asyncio.run(main())
