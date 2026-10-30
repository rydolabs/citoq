#!/usr/bin/env python3
"""Multi-publisher public CitoQ, Zenoh peer and CycloneDDS discovery probe.

Each process lives in an independent Linux netns. The workload is
P publishers (distinct topic per publisher) -> S subscribers (each interested
in every publisher) plus one negative-interest subscriber.

Every publisher must independently discover its matching subscribers.
All P publishers synchronize on file-backed readiness markers before sending.
Payloads and (publisher, sequence) identities are checked by every subscriber.
"""
import argparse
from contextlib import ExitStack
from dataclasses import dataclass
import json
from pathlib import Path
import struct
import sys
import threading
import time

PAYLOAD_SIZE = 256
BASE_TYPE = 424242
RESOURCE = "robot_1"
TOPIC_PREFIX = "citoq/discovery/multi"
NEGATIVE_TYPE = BASE_TYPE + 10000
NEGATIVE_TOPIC = "citoq/discovery/multi/unrelated"
DDS_TYPENAME = "CitoQMultiDiscovery.BenchMsg"
FILL = b"A" * 247


def now():
    return time.monotonic_ns()


def packet(pub, seq):
    return b"D" + struct.pack("<Q", (pub << 32) | seq) + FILL


def decode(raw):
    raw = bytes(raw)
    if len(raw) != PAYLOAD_SIZE or raw[:1] != b"D" or raw[9:] != FILL:
        return None
    tag, = struct.unpack_from("<Q", raw, 1)
    return (tag >> 32, tag & 0xffffffff)


class Collector:
    def __init__(self, publishers, samples, negative):
        self.publishers = publishers
        self.samples = samples
        self.negative = negative
        self.lock = threading.Lock()
        self.seen = set()
        self.invalid = 0
        self.duplicates = 0
        self.first_ns = 0
        self.first_by_pub_ns = {}

    def accept(self, pub_topic, raw):
        parsed = decode(raw)
        t = now()
        with self.lock:
            if self.negative or parsed is None:
                self.invalid += 1
                return
            pub, seq = parsed
            if (pub != pub_topic or not (0 <= pub < self.publishers)
                    or not (1 <= seq <= self.samples)):
                self.invalid += 1
                return
            if (pub, seq) in self.seen:
                self.duplicates += 1
                return
            self.seen.add((pub, seq))
            if not self.first_ns:
                self.first_ns = t
                print(f"BENCH_FIRST_DATA ns={t}", flush=True)
            if pub not in self.first_by_pub_ns:
                self.first_by_pub_ns[pub] = t
                print(f"BENCH_FIRST_PUBLISHER_DATA pub={pub} ns={t}",
                      flush=True)

    def received(self):
        with self.lock:
            return len(self.seen)

    def report(self):
        with self.lock:
            per_pub = {str(pub): sum(1 for p, _ in self.seen if p == pub)
                       for pub in range(self.publishers)}
            missing = {str(pub): [seq for seq in range(1, self.samples+1)
                                  if (pub, seq) not in self.seen]
                       for pub in range(self.publishers)}
            return dict(received=len(self.seen), duplicates=self.duplicates,
                        invalid=self.invalid, first_received_ns=self.first_ns,
                        first_by_pub_ns={str(k):v for k,v in self.first_by_pub_ns.items()},
                        per_publisher=per_pub, missing_by_publisher=missing)


def topic(p):
    return f"{TOPIC_PREFIX}/p{p:03d}"


def wait_for_all_ready(args, poll=None):
    folder = Path(args.output)
    timeout = time.monotonic() + args.discovery_timeout_ms/1000
    announced = False
    while time.monotonic() < timeout:
        files = [folder / f"pub-{i:03d}-ready.marker"
                 for i in range(args.publishers)]
        if all(f.exists() for f in files):
            print("BENCH_ALL_PUBLISHERS_READY", flush=True)
            time.sleep(.3)
            return True
        if poll:
            poll()
        if not announced:
            announced = True
            print("BENCH_SYNC_WAIT publishers="+str(args.publishers),
                  flush=True)
        time.sleep(.004)
    return False


def observe_receivers(args, collector, poll=None, take=None):
    deadline = time.monotonic() + args.lifetime_ms/1000
    required = args.publishers*args.count
    last_progress = time.monotonic()
    while time.monotonic() < deadline:
        if poll:
            poll()
        if take:
            take()
        if args.role == "subscriber" and collector.received() == required:
            end = time.monotonic() + .5
            while time.monotonic() < end:
                if poll:
                    poll()
                if take:
                    take()
                time.sleep(.002)
            break
        if args.role == "negative":
            markers = [Path(args.output) / f"pub-{i:03d}-done.marker"
                       for i in range(args.publishers)]
            if all(p.exists() for p in markers):
                end = time.monotonic() + .5
                while time.monotonic() < end:
                    if poll:
                        poll()
                    if take:
                        take()
                    time.sleep(.002)
                break
        if time.monotonic() - last_progress >= 5:
            print(f"RECEIVER_PROGRESS index={args.index} "
                  f"received={collector.received()} expected={required}",
                  flush=True)
            last_progress = time.monotonic()
        time.sleep(.002)


def publish_frames(args, publish, poll=None):
    if not wait_for_all_ready(args, poll):
        raise RuntimeError("not all publishers reached matching readiness")
    print(f"BENCH_PUBLISH_STARTED index={args.index} ns={now()}", flush=True)
    for seq in range(1, args.count+1):
        publish(packet(args.index, seq))
        if poll:
            poll()
        time.sleep(.10)
    print(f"BENCH_PUBLISH_FINISHED index={args.index} count={args.count}",
          flush=True)


def citoq_run(args, collector):
    import citoq
    from citoq.native_runtime import NativeRuntime
    pub = args.role == "publisher"
    opts = dict(
        coordinator=200+args.index, incarnation=100200+args.index,
        destination=0 if pub else 700+args.index,
        control_port=47465 if pub else 47600,
        quic_port=0 if pub else 4465,
        announcement_ipv4=None if pub else "10.202.0.255",
        announcement_port=0 if pub else 47465,
        trusted_server_ca=args.ca,
        server_cert=None if pub else args.server_cert,
        server_key=None if pub else args.server_key,
        client_ca=None if pub else args.ca,
        client_cert=args.client_cert if pub else None,
        client_key=args.client_key if pub else None)
    with NativeRuntime(**opts) as transport, citoq.Context("acme") as ctx:
        if not pub:
            for p in range(args.publishers):
                def handler(raw, pub_idx=p):
                    # CitoQ publish_raw() returns the subscriber count, so
                    # readiness probes are real control-purpose publications.
                    # They are NOT part of the 8-frame D-prefixed payload set.
                    # Preserve invalid accounting for malformed D frames.
                    if bytes(raw).startswith(b"P"):
                        return
                    collector.accept(pub_idx, raw)
                if args.role == "subscriber":
                    ctx.subscribe_raw(BASE_TYPE+p, handler, RESOURCE)
            if args.role == "negative":
                ctx.subscribe_raw(NEGATIVE_TYPE,
                                  lambda raw: collector.accept(-1, raw),
                                  RESOURCE)
            print("RECEIVER_READY", flush=True)
            observe_receivers(args, collector, poll=transport.poll)
            return dict(matched=None, ready_ms=None)
        ctx.advertise_raw(BASE_TYPE+args.index, RESOURCE)
        start = now()
        deadline = time.monotonic()+args.discovery_timeout_ms/1000
        matched = 0
        probe_seq = 0
        while time.monotonic() < deadline:
            transport.poll()
            probe_seq += 1
            matched = ctx.publish_raw(BASE_TYPE+args.index,
                                      b"P"+struct.pack("<Q", probe_seq),
                                      RESOURCE)
            if matched == args.subscribers:
                break
            time.sleep(.03)
        ready_ms = (now()-start)/1e6 if matched == args.subscribers else None
        if ready_ms is None:
            return dict(matched=matched, ready_ms=None, sent=0)
        print(f"PUBLIC_DISCOVERY_READY elapsed_ms={ready_ms:.3f} "
              f"matched={matched}", flush=True)
        (Path(args.output)/f"pub-{args.index:03d}-ready.marker").write_text(
            str(now()))
        sent = 0
        def put(raw):
            nonlocal sent
            remote = ctx.publish_raw(BASE_TYPE+args.index, raw, RESOURCE)
            if remote != args.subscribers:
                raise RuntimeError(
                    f"pub {args.index}: route count {remote} not {args.subscribers}")
            sent += 1
        publish_frames(args, put, transport.poll)
        end = time.monotonic()+2.0
        while time.monotonic()<end:
            transport.poll()
            time.sleep(.002)
        return dict(matched=matched, ready_ms=ready_ms, sent=sent)


def zenoh_run(args, collector):
    import zenoh
    cfg = zenoh.Config()
    cfg.insert_json5("mode", '"peer"')
    cfg.insert_json5("scouting/multicast/enabled", "true")
    cfg.insert_json5("scouting/multicast/interface", '"eth0"')
    cfg.insert_json5("scouting/gossip/enabled", "true")
    cfg.insert_json5("transport/shared_memory/enabled", "false")
    cfg.insert_json5("transport/link/protocols", '["tcp"]')
    pub = args.role == "publisher"
    with zenoh.open(cfg) as session:
        if not pub:
            with ExitStack() as stack:
                def listener(pub_idx):
                    def recv(sample):
                        try:
                            collector.accept(pub_idx,
                                             sample.payload.to_bytes())
                        except Exception:
                            with collector.lock:
                                collector.invalid += 1
                    return recv
                if args.role == "negative":
                    stack.enter_context(session.declare_subscriber(
                        NEGATIVE_TOPIC, listener(-1)))
                else:
                    for p in range(args.publishers):
                        stack.enter_context(session.declare_subscriber(
                            topic(p), listener(p)))
                print("RECEIVER_READY", flush=True)
                observe_receivers(args, collector)
            return dict(ready_ms=None, matched=None)
        with session.declare_publisher(topic(args.index)) as writer:
            # Require direct links to all N-1 peers, including unrelated peer:
            # unlike matching_status this is a real connection count.
            start = now()
            deadline = time.monotonic()+args.discovery_timeout_ms/1000
            links = 0
            progress = time.monotonic()
            while time.monotonic()<deadline:
                links = len(session.info.peers_zid())
                if links >= args.nodes-1:
                    break
                if time.monotonic()-progress >= 5:
                    print(f"ZENOH_CONNECTED_PEERS index={args.index} "
                          f"count={links} expected={args.nodes-1}",
                          flush=True)
                    progress = time.monotonic()
                time.sleep(.05)
            ready_ms = ((now()-start)/1e6
                        if links >= args.nodes-1 else None)
            if ready_ms is None:
                return dict(matched=None, links=links,
                            expected_links=args.nodes-1, ready_ms=None, sent=0)
            print(f"PUBLIC_DISCOVERY_READY elapsed_ms={ready_ms:.3f} "
                  f"connected_peers={links}", flush=True)
            (Path(args.output)/f"pub-{args.index:03d}-ready.marker").write_text(
                str(now()))
            sent = 0
            def put(raw):
                nonlocal sent
                writer.put(raw)
                sent += 1
            publish_frames(args, put)
            time.sleep(1.5)
            return dict(matched=None, links=links,
                        expected_links=args.nodes-1, ready_ms=ready_ms,
                        sent=sent)


def cyclonedds_run(args, collector):
    from cyclonedds.domain import DomainParticipant
    from cyclonedds.topic import Topic
    from cyclonedds.pub import DataWriter
    from cyclonedds.sub import DataReader
    from cyclonedds.idl import IdlStruct
    from cyclonedds.idl.types import uint8, sequence
    from cyclonedds.qos import Qos, Policy

    @dataclass
    class BenchMsg(IdlStruct, typename=DDS_TYPENAME):
        payload: sequence[uint8]

    dp = DomainParticipant(22)
    qos = Qos(Policy.Reliability.Reliable(1_000_000_000),
              Policy.History.KeepLast(1024))
    pub = args.role == "publisher"
    if not pub:
        readers = []
        for p in range(args.publishers):
            name = NEGATIVE_TOPIC if args.role == "negative" else topic(p)
            if args.role == "negative" and p > 0:
                break
            reader = DataReader(dp, Topic(dp, name, BenchMsg), qos=qos)
            readers.append((p if args.role == "subscriber" else -1, reader))
        print("RECEIVER_READY", flush=True)
        def take():
            for p, reader in readers:
                for sample in reader.take(N=128):
                    if isinstance(sample, BenchMsg):
                        collector.accept(p, sample.payload)
        observe_receivers(args, collector, take=take)
        return dict(ready_ms=None, matched=None)

    writer = DataWriter(dp, Topic(dp, topic(args.index), BenchMsg), qos=qos)
    start = now()
    deadline = time.monotonic()+args.discovery_timeout_ms/1000
    matched = 0
    progress = time.monotonic()
    while time.monotonic()<deadline:
        matched = len(writer.get_matched_subscriptions())
        if matched >= args.subscribers:
            break
        if time.monotonic()-progress >= 5:
            print(f"DDS_MATCHED index={args.index} count={matched} "
                  f"expected={args.subscribers}", flush=True)
            progress = time.monotonic()
        time.sleep(.05)
    ready_ms = ((now()-start)/1e6
                if matched == args.subscribers else None)
    if ready_ms is None:
        return dict(matched=matched, ready_ms=None, sent=0)
    print(f"PUBLIC_DISCOVERY_READY elapsed_ms={ready_ms:.3f} "
          f"matched={matched}", flush=True)
    (Path(args.output)/f"pub-{args.index:03d}-ready.marker").write_text(
        str(now()))
    sent = 0
    def put(raw):
        nonlocal sent
        writer.write(BenchMsg(list(raw)))
        sent += 1
    publish_frames(args, put)
    ack = writer.wait_for_acks(5_000_000_000)
    print(f"CYCLONEDDS_ACK index={args.index} all={int(bool(ack))}",
          flush=True)
    time.sleep(.3)
    return dict(matched=matched, ready_ms=ready_ms, sent=sent,
                writer_acked=bool(ack))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("role", choices=("publisher", "subscriber", "negative"))
    p.add_argument("--engine", choices=("citoq","zenoh","cyclonedds"),
                   required=True)
    p.add_argument("--index", type=int, required=True)
    p.add_argument("--nodes", type=int, required=True)
    p.add_argument("--publishers", type=int, required=True)
    p.add_argument("--subscribers", type=int, required=True)
    p.add_argument("--count", type=int, default=8)
    p.add_argument("--lifetime-ms", type=int, default=110000)
    p.add_argument("--discovery-timeout-ms", type=int, default=90000)
    p.add_argument("--output", required=True)
    for flag in ("ca","server-cert","server-key","client-cert","client-key"):
        p.add_argument("--"+flag)
    a = p.parse_args()
    if not (30 <= a.nodes <= 100 and
            2 <= a.publishers <= a.nodes-2 and
            a.subscribers == a.nodes-a.publishers-1 and
            0 <= a.index < a.nodes and 1 <= a.count <= 100):
        p.error("invalid multi-publisher layout")
    collector = Collector(a.publishers, a.count, a.role=="negative")
    base = dict(engine=a.engine, role=a.role, index=a.index,
                publisher_count=a.publishers,
                subscriber_count=a.subscribers, samples=a.count)
    try:
        run = {"citoq": citoq_run,
               "zenoh": zenoh_run,
               "cyclonedds": cyclonedds_run}[a.engine]
        detail = run(a, collector)
        if a.role == "publisher" and detail.get("sent",0) == a.count:
            (Path(a.output)/f"pub-{a.index:03d}-done.marker").write_text(
                str(now()))
        result = {**base, **collector.report(), **detail}
        print("NODE_RESULT="+json.dumps(result,sort_keys=True), flush=True)
        if a.role=="publisher":
            return 0 if (detail.get("ready_ms") is not None and
                         detail.get("sent") == a.count) else 4
        if a.role=="negative":
            return 0 if result["received"]==0 and result["invalid"]==0 else 5
        return 0 if (result["received"] == a.publishers*a.count
                     and result["duplicates"]==0
                     and result["invalid"]==0) else 6
    except Exception as exc:
        import traceback
        traceback.print_exc()
        print("NODE_RESULT="+json.dumps({
            **base, **collector.report(), "error":str(exc)},sort_keys=True),
            flush=True)
        return 10

if __name__=="__main__":
    sys.exit(main())
