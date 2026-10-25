#!/usr/bin/env python3
"""Zenoh peer-mode native binding A/B driver; same nominal application bytes as M26.
Zenoh's default TCP/peer reliability and DDS/CitoQ UDP BEST_EFFORT are NOT QoS equivalent.
"""
import json
import struct
import sys
import threading
import time

import zenoh


def monotonic():
    return time.monotonic_ns()


def pct95(values):
    return sorted(values)[(95 * len(values) + 99) // 100 - 1] if values else 0


def main():
    if len(sys.argv) != 11:
        raise RuntimeError("args: role topics bytes samples hz unused lifetime_ms index nodes percent")
    role = sys.argv[1]
    topics, size, samples, hz, lifetime = map(int, (sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[7]))
    index, nodes, percent = map(int, (sys.argv[8], sys.argv[9], sys.argv[10]))
    if not (role in {"publisher","subscriber","negative"} and 1 <= topics <= 256 and 12 <= size <= 60000
            and 1 <= samples <= 10000 and 1 <= hz <= 2000 and nodes >= 3 and 1 <= percent <= 100):
        raise RuntimeError("invalid benchmark arguments")
    active = max(1, (topics * percent + 99) // 100)
    subscribed = active
    lock = threading.Lock()
    seen = set()
    received = 0
    bad = 0
    dup = 0
    latencies = []
    started = monotonic()
    sent = 0
    ready = False

    conf = zenoh.Config()
    conf.insert_json5("mode", '"peer"')
    # Multicast discovery across the Linux bridge; do not secretly attach a local router.
    conf.insert_json5("scouting/multicast/enabled", "true")
    with zenoh.open(conf) as session:
        pubs = []
        subs = []

        def topic(t):
            return f"citoq/bench/t{t:04d}"

        def listener(t):
            def on_sample(sample):
                nonlocal received, bad, dup
                raw = sample.payload.to_bytes()
                now = monotonic()
                with lock:
                    if role == "negative" or len(raw) != size:
                        bad += 1
                        return
                    seq, when = struct.unpack_from("<IQ", raw)
                    if seq >= samples or when > now or raw[12:] != bytes([65+t%23]) * (size-12):
                        bad += 1
                        return
                    key = (t, seq)
                    if key in seen:
                        dup += 1
                    else:
                        seen.add(key)
                        received += 1
                        latencies.append((now-when)//1000)
            return on_sample

        if role == "publisher":
            for t in range(topics):
                pubs.append(session.declare_publisher(topic(t)))
            start_send = None
            while (monotonic()-started)//1000000 < lifetime:
                now = monotonic()
                if not ready and all(bool(p.matching_status) for p in pubs[:subscribed]):
                    ready = True
                    start_send = now + 500_000_000
                    print(f"BENCH_DISCOVERY_READY elapsed_ms={(now-started)//1000000}", flush=True)
                if ready and sent < topics * samples and now >= start_send and now-start_send >= sent*1_000_000_000//(hz*topics):
                    t = sent % topics
                    seq = sent // topics
                    payload = struct.pack("<IQ", seq, monotonic()) + bytes([65+t%23]) * (size-12)
                    pubs[t].put(payload)
                    sent += 1
                if ready and sent == topics*samples and now-start_send > samples*1_000_000_000//hz+500_000_000:
                    break
                time.sleep(0.0002)
        else:
            ts = [999999] if role == "negative" else list(range(active))
            for t in ts:
                subs.append(session.declare_subscriber(topic(t), listener(t)))
            expected = active * samples
            while (monotonic()-started)//1000000 < lifetime:
                with lock:
                    complete = received >= expected
                if role != "negative" and complete:
                    break
                time.sleep(0.008)

        for sub in subs:
            sub.undeclare()
        for pub in pubs:
            pub.undeclare()

    output = dict(role=role,topics=topics,bytes=size,samples=samples,sent=sent,
        received=received,bad=bad,duplicate=dup,p95_us=pct95(latencies),
        ready=int(ready),index=index,active_topics=subscribed)
    print(json.dumps(output),flush=True)
    if role == "publisher":
        return 0 if ready and sent == topics*samples else 3
    if role == "negative":
        return 0 if received == 0 and bad == 0 else 4
    return 0 if received <= subscribed*samples and bad == 0 and dup == 0 else 5

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"ZENOH_DRIVER_ERROR: {type(e).__name__}: {e}",file=sys.stderr,flush=True)
        raise
