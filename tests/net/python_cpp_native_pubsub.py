#!/usr/bin/env python3
"""Native Python ↔ public C++ Context: matching TypeId, RPC and typed PubSub."""
import argparse
import asyncio
import struct
import time
import cito
from cito.native_runtime import NativeRuntime

class NetPong:
    def __init__(self, value: int):
        self.value = value
    @staticmethod
    def decode(payload: bytes):
        if len(payload) != 4: raise ValueError("bad reply")
        return NetPong(struct.unpack("<I", payload)[0])
    def encode(self):
        return struct.pack("<I", self.value)

class NetPing:
    TYPE_ID = 0
    REPLY_TYPE = NetPong
    def __init__(self, value: int):
        self.value = value
    @staticmethod
    def decode(payload: bytes):
        if len(payload) != 4: raise ValueError("bad request")
        return NetPing(struct.unpack("<I", payload)[0])
    def encode(self):
        return struct.pack("<I", self.value)

def args_parser():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["server","client"])
    p.add_argument("--type-id", type=int, required=True)
    p.add_argument("--control-port", type=int, required=True)
    p.add_argument("--quic-port", type=int, default=0)
    p.add_argument("--announce-ip")
    p.add_argument("--announce-port", type=int, default=0)
    p.add_argument("--cert")
    p.add_argument("--key")
    p.add_argument("--ca", required=True)
    p.add_argument("--client-cert")
    p.add_argument("--client-key")
    return p.parse_args()

async def main():
    a=args_parser()
    NetPing.TYPE_ID=a.type_id
    server=a.mode == "server"
    with NativeRuntime(
        coordinator=50 if server else 20,
        incarnation=5001 if server else 2001,
        destination=77 if server else 0,
        control_port=a.control_port,
        quic_port=a.quic_port,
        announcement_ipv4=a.announce_ip,
        announcement_port=a.announce_port,
        server_cert=a.cert,
        server_key=a.key,
        client_ca=a.ca if server else None,
        trusted_server_ca=a.ca,
        client_cert=a.client_cert,
        client_key=a.client_key,
    ) as transport:
        with cito.Context("acme") as context:
            deadline=time.monotonic()+20
            if server:
                counts={"rpc":0,"duplicate":0}
                received=set()
                def serve(ping):
                    counts["rpc"]+=1
                    return NetPong(ping.value+1)
                def data_received(payload):
                    if len(payload)!=4:
                        counts["duplicate"]+=1
                        return
                    value=struct.unpack("<I",payload)[0]
                    if value not in range(1,11) or value in received:
                        counts["duplicate"]+=1
                    else: received.add(value)
                context.handle(NetPing,serve,"robot_1")
                context.subscribe_raw(NetPing.TYPE_ID,data_received,"robot_1")
                while time.monotonic()<deadline:
                    transport.poll()
                    if len(received)==10 and counts["rpc"]==1: break
                    await asyncio.sleep(.001)
                if len(received)!=10 or counts["rpc"]!=1 or counts["duplicate"]:
                    raise RuntimeError(f"python server failed: {counts} received={len(received)}")
                print("python_cpp_server_rpc=1 pubsub_received=10",flush=True)
            else:
                while not context.has_handler(NetPing.TYPE_ID,"robot_1"):
                    if time.monotonic()>deadline:
                        raise RuntimeError("Python never discovered C++ RPC handler")
                    transport.poll()
                    await asyncio.sleep(.01)
                pending=asyncio.create_task(context.request(NetPing(41),"robot_1"))
                while not pending.done():
                    if time.monotonic()>deadline:
                        raise RuntimeError("Python RPC timed out")
                    transport.poll()
                    await asyncio.sleep(.001)
                result=await pending
                if result.value!=42:
                    raise RuntimeError(f"unexpected Python RPC result={result.value}")
                context.advertise_raw(NetPing.TYPE_ID,"robot_1")
                for seq in range(1,11):
                    while True:
                        if time.monotonic()>deadline:
                            raise RuntimeError(f"Python CTD1 missing session at {seq}")
                        transport.poll()
                        if context.publish_raw(NetPing.TYPE_ID,struct.pack("<I",seq),"robot_1")>0:
                            break
                        await asyncio.sleep(.01)
                    await asyncio.sleep(.001)
                done=time.monotonic()+.6
                while time.monotonic()<done:
                    transport.poll()
                    await asyncio.sleep(.001)
                print("python_cpp_client_rpc=42 pubsub_sent=10",flush=True)

if __name__=="__main__":
    asyncio.run(main())
