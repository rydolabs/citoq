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
            rx_sleep = args.citoq_rx_sleep_us / 1_000_000
            def service_receiver():
                if args.citoq_rx_mode == "event":
                    transport.poll_wait(args.citoq_rx_wait_ms)
                else:
                    transport.poll()
                    if rx_sleep:
                        time.sleep(rx_sleep)
            while time.monotonic()<until and len(collector.seen)<args.count:
                service_receiver()
            for _ in range(100):
                service_receiver()
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
    # Zenoh 1.10 auto-promotes >=3072-byte same-host messages to SHM.
    # Linux netns share IPC: force the actual TCP/veth path for network A/B.
    cfg.insert_json5("transport/shared_memory/enabled", "false")
    if args.zenoh_transport == "quic-mtls":
        # Same native QUIC+mTLS trust roots, signed identities, payloads and
        # veth namespaces as the CitoQ control. This is security/transport
        # normalized, NOT identical internal QoS, stream/framing or runtime.
        cfg.insert_json5("transport/link/protocols", '["quic"]')
        tls = {
            "root_ca_certificate": str(args.ca),
            "enable_mtls": True,
            "verify_name_on_connect": True,
        }
        if args.role == "receiver":
            tls.update(listen_certificate=str(args.cert),
                       listen_private_key=str(args.key))
        else:
            tls.update(connect_certificate=str(args.client_cert),
                       connect_private_key=str(args.client_key))
        cfg.insert_json5("transport/link/tls", json.dumps(tls))
        protocol = "quic"
    else:
        cfg.insert_json5("transport/link/protocols", '["tcp"]')
        protocol = "tcp"
    # Peer mode may inherit a default TCP listening endpoint. Reset both
    # endpoint collections so our QUIC-only protocol allowlist cannot conflict
    # with an implicit TCP listener, or accidentally permit TCP fallback.
    cfg.insert_json5("listen/endpoints", "[]")
    cfg.insert_json5("connect/endpoints", "[]")
    if args.role=="receiver":
        cfg.insert_json5("listen/endpoints", json.dumps([f"{protocol}/0.0.0.0:7447"]))
    else:
        cfg.insert_json5("connect/endpoints", json.dumps([f"{protocol}/10.217.0.2:7447"]))
    print("ZENOH_TRANSPORT=" + args.zenoh_transport, flush=True)
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
                if not isinstance(sample, BenchMsg):
                    # DDS invalid/disposed instance event is not a received data sample.
                    continue
                collector.ingest(int(sample.seq),int(sample.sent_ns),len(sample.payload)+16)
            time.sleep(.0005)
        for sample in reader.take(N=256):
            if not isinstance(sample, BenchMsg):
                continue
            collector.ingest(int(sample.seq),int(sample.sent_ns),len(sample.payload)+16)
    else:
        writer=DataWriter(dp,topic,qos=qos)
        # Reliable DDS late-join safety: do not write historical samples
        # before an actual subscriber has matched.
        match_deadline=time.monotonic()+args.timeout
        while time.monotonic()<match_deadline and not writer.get_matched_subscriptions():
            time.sleep(.02)
        if not writer.get_matched_subscriptions():
            raise RuntimeError("CycloneDDS writer was not matched before sending")
        def send(raw):
            seq,stamp=unpack_payload(raw)
            writer.write(BenchMsg(seq,stamp,list(raw[16:])))
            return 1
        paced_send(args,send)
        # Reliable DDS writers must remain alive to retransmit until ACKed,
        # rather than being destroyed immediately after the final write.
        acked = writer.wait_for_acks(5_000_000_000)
        print(f"CYCLONEDDS_WRITER_ACKED={int(acked)}", flush=True)
        if not acked:
            # Preserve the evidence: failure remains a failure, not a hidden success.
            raise RuntimeError("CycloneDDS reliable writer ACK timeout")


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
                # publish_raw() queues a QUIC send on the loop owner. Without
                # this poll the queued packet sits until the *next* 100 Hz
                # publish, adding a full 10 ms to the measured one-way delay.
                if service and args.citoq_poll_mode == "immediate":
                    service()
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
    ap.add_argument("--zenoh-transport",choices=("tcp","quic-mtls"),default="tcp",
                    help="quic-mtls restricts links to QUIC with CA trust and client certificate authentication")
    ap.add_argument("--citoq-poll-mode",choices=("immediate","legacy"),default="immediate",
                    help="immediate services queued QUIC send after publish; legacy reproduces the 10ms benchmark bug")
    ap.add_argument("--citoq-rx-sleep-us",type=int,default=500,
                    help="receiver poll-loop sleep in microseconds; 0 means busy polling (diagnostic only)")
    ap.add_argument("--citoq-rx-mode",choices=("sleep","event"),default="sleep",
                    help="event uses kernel wakeup + owner-thread poll_wait without busy spin")
    ap.add_argument("--citoq-rx-wait-ms",type=int,default=10,
                    help="maximum time for event-driven poll_wait")
    ap.add_argument("--ca")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--client-cert")
    ap.add_argument("--client-key")
    ap.add_argument("--result")
    args=ap.parse_args()
    if (args.count<1 or args.payload<16 or args.rate<=0 or
            not 0<=args.citoq_rx_sleep_us<=1_000_000 or
            not 0<=args.citoq_rx_wait_ms<=1000):
        ap.error("count>0, payload>=16, rate>0, 0<=rx-sleep-us<=1000000 and 0<=rx-wait-ms<=1000 required")
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
