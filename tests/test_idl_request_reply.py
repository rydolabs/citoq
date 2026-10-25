from __future__ import annotations

import request_reply


def main() -> None:
    assert (
        request_reply.GetStatus.REPLY_TYPE
        is request_reply.Status
    )

    request = request_reply.GetStatus(
        drone_id="drone_17")
    decoded = request_reply.GetStatus.decode(
        request.encode())

    assert decoded == request


if __name__ == "__main__":
    main()
