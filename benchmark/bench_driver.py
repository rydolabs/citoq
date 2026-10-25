#!/usr/bin/env python3
"""Cross-network-namespace one-way Pub/Sub probe.

Two namespaces share the same Linux CLOCK_MONOTONIC clock. The sender embeds
monotonic_ns; the receiver records delta, not network-only latency.
A result is valid only if the receiver exits and writes a parseable report.
"""

import argparse
import json
import os
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

TYPE_ID = 424242
TOPIC = "citoq/bench/telemetry"
STRUCT = struct.Struct("<QQ")


def now_ns():
    return time.monotonic_ns()


def make_payload(seq, size):
    return STRUCT.pack(seq, now_ns()) + b"B" * (size - STRUCT.size)


def unpack_payload(raw):
    raw = bytes(raw)
    if len(raw) < 16:
        raise ValueError("short packet")
    return STRUCT.unpack_from(raw)


@dataclass
class Collector:
    expected: int
    size: int
    seen: dict
    duplicates: int = 0
    invalid: int = 0
    first_ns: int = 0
    last_ns: int = 0

    def ingest(self, seq, sent_ns, data_len):
        receive_ns = now_ns()
        if not (1 <= seq <= self.expected) or data_len != self.size or sent_ns <= 0:
            self.invalid += 1
        elif seq in self.seen:
            self.duplicates += 1
        else:
            delta = receive_ns - sent_ns
            if delta < 0:
                self.invalid += 1
            else:
                self.seen[seq] = delta
                self.first_ns = self.first_ns or receive_ns
                self.last_ns = receive_ns

    def report(self, engine):
        times = sorted(self.seen.values())
        def percentile(p):
            if not times:
                return None
            return times[min(len(times)-1, int(round((len(times)-1)*p)))] / 1000
        return {
            "engine": engine,
            "expected": self.expected,
            "received_unique": len(times),
            "duplicates": self.duplicates,
            "invalid": self.invalid,
            "missing": self.expected-len(times),
            "payload_bytes": self.size,
            "latency_us_p50": percentile(.50),
            "latency_us_p95": percentile(.95),
            "latency_us_p99": percentile(.99),
            "delivery_window_ms": max(0, (self.last_ns-self.first_ns)/1e6),
            "complete": len(times)==self.expected and self.duplicates==0 and self.invalid==0,
        }


def citoq_run(args, collector):
    import citoq
    from citoq.native_runtime import NativeRuntime
    server = args.role == "receiver"
    with NativeRuntime(
        coordinator=50 if server else 20,
        incarnation=5001 if server else 2001,
        destination=77 if server else 0,
        control_port=47600 if server else 47465,
        quic_port=4465 if server else 0,
        announcement_ipv4="10.217.0.1" if server else None,
        announcement_port=47465 if server else 0,
        server_cert=args.cert if server else None,
        server_key=args.key if server else None,
        client_ca=args.ca if server else None,
        trusted_server_ca=args.ca,
        client_cert=args.client_cert if not server else None,
        client_key=args.client_key if not server else None,
    ) as transport, citoq.Context("acme") as ctx:
        if server:
            def receive(raw):
                try:
                    seq, stamp = unpack_payload(raw)
                    collector.ingest(seq, stamp, len(raw))
                except Exception:
                    collector.invalid += 1
            ctx.subscribe_raw(TYPE_ID, receive, "robot_1")
            until=time.monotonic()+args.timeout
            while time.monotonic()<until and len(collector.seen)<args.count:
                transport.poll()
                time.sleep(.0005)
            for _ in range(100):
                transport.poll()
                time.sleep(.0005)
        else:
            ctx.advertise_raw(TYPE_ID, "robot_1")
            paced_send(args, lambda b: ctx.publish_raw(TYPE_ID, b, "robot_1"),
                       service=transport.poll, wait_for_match=True)
            until=time.monotonic()+1.0
            while time.monotonic()<until:
                transport.poll()
                time.sleep(.0005)


def zenoh_run(args, collector):
    import zenoh
    cfg=zenoh.Config()
    cfg.insert_json5("mode", '"peer"')
    cfg.insert_json5("scouting/multicast/enabled", "false")
    if args.role=="receiver":
        cfg.insert_json5("listen/endpoints", '["tcp/0.0.0.0:7447"]')
    else:
        cfg.insert_json5("connect/endpoints", '["tcp/10.217.0.2:7447"]')
    with zenoh.open(cfg) as session:
        if args.role=="receiver":
            def receive(sample):
                try:
                    raw=sample.payload.to_bytes()
                    seq,stamp=unpack_payload(raw)
                    collector.ingest(seq,stamp,len(raw))
                except Exception:
                    collector.invalid+=1
            with session.declare_subscriber(TOPIC, receive):
                wait_receive(args,collector)
        else:
            with session.declare_publisher(TOPIC) as pub:
                paced_send(args,lambda b: (pub.put(b),1)[1])


def cyclone_run(args, collector):
    from cyclonedds.domain import DomainParticipant
    from cyclonedds.topic import Topic
    from cyclonedds.pub import DataWriter
    from cyclonedds.sub import DataReader
    from cyclonedds.idl import IdlStruct
    from cyclonedds.idl.types import uint64, uint8, sequence
    from cyclonedds.qos import Qos, Policy

    @dataclass
    class BenchMsg(IdlStruct, typename="CitoQBenchmark.BenchMsg"):
        seq: uint64
        sent_ns: uint64
        payload: sequence[uint8]

    dp=DomainParticipant(22)
    topic=Topic(dp,"CitoQBenchmark",BenchMsg)
    qos=Qos(Policy.Reliability.Reliable(1_000_000_000),
            Policy.History.KeepLast(1024))
    if args.role=="receiver":
        reader=DataReader(dp,topic,qos=qos)
        until=time.monotonic()+args.timeout
        while time.monotonic()<until and len(collector.seen)<args.count:
            for sample in reader.take(N=128):
                collector.ingest(int(sample.seq),int(sample.sent_ns),len(sample.payload)+16)
            time.sleep(.0005)
        for sample in reader.take(N=256):
            collector.ingest(int(sample.seq),int(sample.sent_ns),len(sample.payload)+16)
    else:
        writer=DataWriter(dp,topic,qos=qos)
        def send(raw):
            seq,stamp=unpack_payload(raw)
            writer.write(BenchMsg(seq,stamp,list(raw[16:])))
            return 1
        paced_send(args,send)


def paced_send(args, fn, service=None, wait_for_match=False):
    next_ns=now_ns()
    interval=int(1e9/args.rate)
    for n in range(1,args.count+1):
        deadline=time.monotonic()+args.timeout
        while True:
            if service:
                service()
            success=fn(make_payload(n,args.payload))
            if success:
                break
            if not wait_for_match or time.monotonic()>deadline:
                raise RuntimeError("no eligible subscribers for sample "+str(n))
            time.sleep(.005)
        next_ns+=interval
        delay=(next_ns-now_ns())/1e9
        if delay>0: time.sleep(delay)
    print(f"SENDER_OK engine={args.engine} sent={args.count}",flush=True)


def wait_receive(args,collector):
    until=time.monotonic()+args.timeout
    while time.monotonic()<until and len(collector.seen)<args.count:
        time.sleep(.002)
    time.sleep(.08)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("engine",choices=["citoq","zenoh","cyclonedds"])
    ap.add_argument("role",choices=["receiver","sender"])
    ap.add_argument("--count",type=int,default=200)
    ap.add_argument("--payload",type=int,default=256)
    ap.add_argument("--rate",type=float,default=100)
    ap.add_argument("--timeout",type=float,default=16)
    ap.add_argument("--ca")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--client-cert")
    ap.add_argument("--client-key")
    ap.add_argument("--result")
    args=ap.parse_args()
    if args.count<1 or args.payload<16 or args.rate<=0:
        ap.error("count>0, payload>=16, rate>0 required")
    collector=Collector(args.count,args.payload,{})
    engine={"citoq":citoq_run,"zenoh":zenoh_run,"cyclonedds":cyclone_run}[args.engine]
    try:
        engine(args,collector)
        if args.role=="receiver":
            report=collector.report(args.engine)
            print("BENCH_JSON="+json.dumps(report,sort_keys=True),flush=True)
            if args.result:
                Path(args.result).write_text(json.dumps(report,indent=2)+"\n")
            return 0
        return 0
    except Exception as exc:
        import traceback
        traceback.print_exc()
        if args.role=="receiver" and args.result:
            r=collector.report(args.engine)
            r["error"]=repr(exc)
            Path(args.result).write_text(json.dumps(r,indent=2)+"\n")
        return 2

if __name__=="__main__":
    sys.exit(main())
