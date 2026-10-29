#!/usr/bin/env python3
"""Zenoh peer gossip+multicast and CycloneDDS SPDP/SEDP public API discovery probe.

Matches public_discovery_node.py topology/payload/metrics. Note: unlike public
CitoQ MsQuic mTLS, Zenoh uses peer TCP and CycloneDDS uses DDS/RTPS UDP.
Transport/security parity is NOT claimed.
"""
import argparse
import json
import pathlib
import struct
import sys
import threading
import time

SIZE=256
COUNT_PREFIX=b"D"
FILL=b"A"*247
TOPIC="citoq/discovery/benchmark"
NEGATIVE_TOPIC="citoq/discovery/unrelated"
TYPE_NAME="CitoQDiscoveryScale.BenchMsg"

def now():
    return time.monotonic_ns()

def payload(sequence):
    return COUNT_PREFIX+struct.pack("<Q",sequence)+FILL

class Collector:
    def __init__(self, count, negative):
        self.count=count
        self.negative=negative
        self.lock=threading.Lock()
        self.seen=set()
        self.duplicates=0
        self.invalid=0
        self.first_ns=0

    def accept(self, raw):
        buf=bytes(raw)
        t=now()
        with self.lock:
            if (self.negative or len(buf)!=SIZE or buf[0:1]!=COUNT_PREFIX or
                    buf[9:]!=FILL):
                self.invalid+=1
                return
            seq=struct.unpack_from("<Q",buf,1)[0]
            if not 1<=seq<=self.count:
                self.invalid+=1
                return
            if seq in self.seen:
                self.duplicates+=1
                return
            self.seen.add(seq)
            if not self.first_ns:
                self.first_ns=t
                print("BENCH_FIRST_DATA ns="+str(t),flush=True)

    def count_received(self):
        with self.lock:
            return len(self.seen)

    def report(self):
        with self.lock:
            return dict(received=len(self.seen),duplicates=self.duplicates,
                        invalid=self.invalid,first_received_ns=self.first_ns)

def observe(collector, role, until, completion):
    marker=pathlib.Path(completion) if completion else None
    grace=None
    while time.monotonic()<until:
        if role=="subscriber" and collector.count_received()==collector.count:
            grace=time.monotonic()+.2
            break
        if role=="negative" and marker and marker.exists():
            grace=time.monotonic()+.3
            break
        time.sleep(.004)
    if grace is not None:
        while time.monotonic()<grace:
            time.sleep(.004)

def sender_run(args, matched_count):
    started=now()
    deadline=time.monotonic()+args.discovery_timeout_ms/1000
    ready=False
    match_value=None
    while time.monotonic()<deadline:
        match_value=matched_count()
        if match_value:
            ready=True
            break
        time.sleep(.04)
    elapsed=(now()-started)/1e6 if ready else None
    if ready:
        print("PUBLIC_DISCOVERY_READY elapsed_ms="+str(round(elapsed,3)),flush=True)
    return elapsed,match_value

def zenoh_run(args,collector):
    import zenoh
    cfg=zenoh.Config()
    cfg.insert_json5("mode",'"peer"')
    cfg.insert_json5("scouting/multicast/enabled","true")
    cfg.insert_json5("scouting/multicast/interface",'"eth0"')
    cfg.insert_json5("scouting/gossip/enabled","true")
    cfg.insert_json5("transport/shared_memory/enabled","false")
    cfg.insert_json5("transport/link/protocols",'["tcp"]')
    pub=args.role=="publisher"
    topic=NEGATIVE_TOPIC if args.role=="negative" else TOPIC
    print("ZENOH_DISCOVERY=peer-multicast-gossip-tcp",flush=True)
    with zenoh.open(cfg) as session:
        if not pub:
            def on_sample(sample):
                try:
                    collector.accept(sample.payload.to_bytes())
                except Exception:
                    with collector.lock:collector.invalid+=1
            with session.declare_subscriber(topic,on_sample):
                print("RECEIVER_READY",flush=True)
                observe(collector,args.role,
                        time.monotonic()+args.lifetime_ms/1000,args.completion_file)
            return dict(matched=None,ready_ms=None,
                        match_semantics="receiver-side all-data acceptance")
        with session.declare_publisher(topic) as writer:
            elapsed,status=sender_run(args,lambda:bool(writer.matching_status))
            if elapsed is None:return dict(matched=None,ready_ms=None,
                                            match_semantics="zenoh boolean any match")
            time.sleep(.5)
            for seq in range(1,args.count+1):
                writer.put(payload(seq))
                time.sleep(.1)
            time.sleep(1.5)
        return dict(matched=None,ready_ms=elapsed,
                    matching_any=bool(status),
                    match_semantics="zenoh boolean any match, NOT N-2 count")

def dds_run(args,collector):
    from dataclasses import dataclass
    from cyclonedds.domain import DomainParticipant
    from cyclonedds.topic import Topic
    from cyclonedds.pub import DataWriter
    from cyclonedds.sub import DataReader
    from cyclonedds.idl import IdlStruct
    from cyclonedds.idl.types import uint8,sequence
    from cyclonedds.qos import Qos,Policy
    @dataclass
    class BenchMsg(IdlStruct,typename=TYPE_NAME):
        payload: sequence[uint8]
    dp=DomainParticipant(22)
    topic=Topic(dp,NEGATIVE_TOPIC if args.role=="negative" else TOPIC,BenchMsg)
    qos=Qos(Policy.Reliability.Reliable(1_000_000_000),
            Policy.History.KeepLast(1024))
    if args.role!="publisher":
        reader=DataReader(dp,topic,qos=qos)
        print("RECEIVER_READY",flush=True)
        until=time.monotonic()+args.lifetime_ms/1000
        marker=pathlib.Path(args.completion_file) if args.completion_file else None
        grace=None
        while time.monotonic()<until:
            for sample in reader.take(N=128):
                if not isinstance(sample,BenchMsg):
                    continue
                try:
                    collector.accept(sample.payload)
                except Exception:
                    with collector.lock:collector.invalid+=1
            if args.role=="subscriber" and collector.count_received()==args.count:
                grace=time.monotonic()+.2
                break
            if args.role=="negative" and marker and marker.exists():
                grace=time.monotonic()+.3
                break
            time.sleep(.003)
        if grace:
            while time.monotonic()<grace:
                for sample in reader.take(N=128):
                    if isinstance(sample,BenchMsg):collector.accept(sample.payload)
                time.sleep(.003)
        return dict(matched=None,ready_ms=None,
                    match_semantics="reader-side all-data acceptance")
    writer=DataWriter(dp,topic,qos=qos)
    def matched():
        return len(writer.get_matched_subscriptions())
    started=now()
    deadline=time.monotonic()+args.discovery_timeout_ms/1000
    count=0
    while time.monotonic()<deadline:
        count=matched()
        if count>=args.nodes-2:break
        time.sleep(.04)
    elapsed=(now()-started)/1e6 if count>=args.nodes-2 else None
    if elapsed is None:return dict(matched=count,ready_ms=None,
                                    match_semantics="DDS exact matched subscriptions")
    print("PUBLIC_DISCOVERY_READY elapsed_ms="+str(round(elapsed,3)),flush=True)
    time.sleep(.5)
    for seq in range(1,args.count+1):
        data=payload(seq)
        writer.write(BenchMsg(list(data)))
        time.sleep(.1)
    ack=writer.wait_for_acks(5_000_000_000)
    print("CYCLONEDDS_WRITER_ACKED="+str(int(ack)),flush=True)
    time.sleep(.3)
    return dict(matched=count,ready_ms=elapsed,acked=bool(ack),
                match_semantics="DDS exact matched subscriptions")

def main():
    p=argparse.ArgumentParser()
    p.add_argument("role",choices=("publisher","subscriber","negative"))
    p.add_argument("--engine",choices=("zenoh","cyclonedds"),required=True)
    p.add_argument("--index",type=int,required=True)
    p.add_argument("--nodes",type=int,required=True)
    p.add_argument("--count",type=int,default=8)
    p.add_argument("--lifetime-ms",type=int,default=105000)
    p.add_argument("--discovery-timeout-ms",type=int,default=90000)
    p.add_argument("--completion-file")
    args=p.parse_args()
    if not (3<=args.nodes<=100 and 0<=args.index<args.nodes and
            1<=args.count<=100):
        p.error("invalid shape")
    collector=Collector(args.count,args.role=="negative")
    base={"role":args.role,"index":args.index,"engine":args.engine,
          "expected":args.count}
    try:
        detail=(zenoh_run(args,collector) if args.engine=="zenoh"
                else dds_run(args,collector))
        if args.role=="publisher" and args.completion_file:
            pathlib.Path(args.completion_file).write_text("finished\n")
        report={**base,**collector.report(),**detail}
        print("NODE_RESULT="+json.dumps(report,sort_keys=True),flush=True)
        if args.role=="publisher":
            # Zenoh does not expose an all-peer match count. The orchestrator
            # independently requires N-2 distinct subscribers to receive all data.
            return 0 if detail.get("ready_ms") is not None and (
                args.engine!="cyclonedds" or detail.get("acked") is True) else 4
        if args.role=="negative":
            return 0 if report["received"]==0 and report["invalid"]==0 else 5
        return 0 if report["received"]==args.count and (
            report["duplicates"]==0 and report["invalid"]==0) else 6
    except Exception as exc:
        import traceback
        traceback.print_exc()
        print("NODE_RESULT="+json.dumps({**base,"error":str(exc),
              **collector.report()}),flush=True)
        return 10

if __name__=="__main__":
    sys.exit(main())
