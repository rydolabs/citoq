#!/usr/bin/env python3
"""Two-process Python PubSub acceptance with native mTLS QUIC driver."""
from __future__ import annotations
import argparse
import struct
import time
import cito
from cito.native_runtime import NativeRuntime

TYPE_ID = 424242
COUNT = 100

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["server", "client"])
    parser.add_argument("--control-port", required=True, type=int)
    parser.add_argument("--quic-port", type=int, default=0)
    parser.add_argument("--announcement-ip")
    parser.add_argument("--announcement-port", type=int, default=0)
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--ca", required=True)
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    args = parser.parse_args()
    server = args.mode == "server"
    with NativeRuntime(
        coordinator=50 if server else 20,
        incarnation=5001 if server else 2001,
        destination=77 if server else 0,
        control_port=args.control_port,
        quic_port=args.quic_port,
        announcement_ipv4=args.announcement_ip,
        announcement_port=args.announcement_port,
        server_cert=args.cert,
        server_key=args.key,
        client_ca=args.ca if server else None,
        trusted_server_ca=args.ca,
        client_cert=args.client_cert,
        client_key=args.client_key,
    ) as transport:
        with cito.Context("acme") as context:
            deadline = time.monotonic() + 20
            if server:
                received: set[int] = set()
                duplicates = [0]
                def handler(data: bytes) -> None:
                    if len(data) != 4:
                        duplicates[0] += 1
                        return
                    value = struct.unpack("<I", data)[0]
                    if value < 1 or value > COUNT or value in received:
                        duplicates[0] += 1
                        return
                    received.add(value)
                context.subscribe_raw(TYPE_ID, handler, "robot_1")
                while time.monotonic() < deadline and len(received) < COUNT:
                    transport.poll()
                    time.sleep(0.001)
                if len(received) != COUNT or duplicates[0]:
                    raise RuntimeError(
                        f"python receive failed count={len(received)} duplicate={duplicates[0]}")
                print(f"python_pubsub_received={len(received)}", flush=True)
            else:
                context.advertise_raw(TYPE_ID, "robot_1")
                for number in range(1, COUNT + 1):
                    while True:
                        if time.monotonic() >= deadline:
                            raise RuntimeError(f"python send failed at seq={number}")
                        transport.poll()
                        delivered = context.publish_raw(
                            TYPE_ID, struct.pack("<I", number), "robot_1")
                        if delivered >= 1:
                            break
                        time.sleep(0.01)
                    time.sleep(0.001)
                stop = time.monotonic() + 0.7
                while time.monotonic() < stop:
                    transport.poll()
                    time.sleep(0.001)
                print(f"python_pubsub_sent={COUNT}", flush=True)

if __name__ == "__main__":
    main()
