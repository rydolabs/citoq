#!/usr/bin/env python3
"""Zenoh peer / router-client benchmark on isolated Linux network namespaces.

Same CLI as the CitoQ and DDS benchmark binaries, plus 'peer' or 'router'.
Each process uses its namespace interface. Publisher node is also peer hub
or zenoh router to avoid hidden out-of-band inter-namespace communication.
"""
import json, struct, sys, time, threading
import zenoh

def main():
    if len(sys.argv)!=12:
        raise RuntimeError("role topics bytes samples hz unused_ip lifetime_ms index nodes percent mode")
    _, role, topics, size, samples, hz, _ip, lifetime_ms, index, nodes, percent, mode = sys.argv
    topics,size,samples,hz,lifetime_ms,index,nodes,percent = map(int,(topics,size,samples,hz,lifetime_ms,index,nodes,percent))
    active=max(1,(topics*percent+99)//100)
    negative=role=="negative"
    is_pub=role=="publisher"
    config=zenoh.Config()
    config.insert_json5("mode",json.dumps("router" if mode=="router" and is_pub else ("client" if mode=="router" else "peer")))
    config.insert_json5("scouting/multicast/enabled","false")
    config.insert_json5("scouting/gossip/enabled","false")
    if is_pub:
        config.insert_json5("listen/endpoints",json.dumps(["tcp/10.202.0.11:7447"]))
    else:
        config.insert_json5("connect/endpoints",json.dumps(["tcp/10.202.0.11:7447"]))
        config.insert_json5("listen/endpoints","[]")
    seen=set()
    lat=[]
    ready_peers=set()
    counters={"sent":0,"received":0,"bad":0,"duplicate":0}
    lock=threading.Lock()
    start=time.monotonic()
    def data_received(sample):
        try:
            key=str(sample.key_expr)
            t=int(key.rsplit("/",1)[1])
            raw=sample.payload.to_bytes()
            if negative or t>=active or len(raw)!=size:
                with lock:counters["bad"]+=1
                return
            seq,stamp=struct.unpack("<IQ",raw[:12])
            if seq>=samples or stamp>time.monotonic_ns() or raw[12:]!=bytes([65+t%23])*(size-12):
                with lock:counters["bad"]+=1
                return
            with lock:
                if (t,seq) in seen:counters["duplicate"]+=1
                else:
                    seen.add((t,seq));counters["received"]+=1
                    lat.append((time.monotonic_ns()-stamp)//1000)
        except Exception:
            with lock:counters["bad"]+=1
    def ready_callback(sample):
        try:
            peer=int(str(sample.key_expr).rsplit("/",1)[1])
            if 1<=peer<nodes-1:
                with lock:ready_peers.add(peer)
        except Exception:pass
    with zenoh.open(config) as session:
        subs=[]
        if is_pub:
            ready_sub=session.declare_subscriber("bench/ready/**",ready_callback)
            pubs=[session.declare_publisher(f"bench/data/{i}") for i in range(topics)]
        else:
            keys=["bench/data/999999"] if negative else [f"bench/data/{i}" for i in range(active)]
            subs=[session.declare_subscriber(key,data_received) for key in keys]
        first_ready=0.0
        last_ready=0.0
        sent=0
        ready=False
        while (time.monotonic()-start)*1000<lifetime_ms:
            now=time.monotonic()
            if is_pub:
                with lock:peers=len(ready_peers)
                if not ready and peers>=nodes-2:
                    ready=True
                    first_ready=now+0.5
                    print(f"BENCH_DISCOVERY_READY elapsed_ms={int((now-start)*1000)}",flush=True)
                if ready and now>=first_ready and sent<topics*samples:
                    for _ in range(32):
                        if sent>=topics*samples:break
                        target=first_ready+sent/(hz*topics)
                        if time.monotonic()<target:break
                        t=sent%topics
                        seq=sent//topics
                        packet=struct.pack("<IQ",seq,time.monotonic_ns())+bytes([65+t%23])*(size-12)
                        pubs[t].put(packet)
                        sent+=1
                        counters["sent"]=sent
                if ready and sent==topics*samples and now-first_ready>samples/hz+0.5:break
            else:
                if not negative and now-last_ready>=0.15:
                    session.put(f"bench/ready/{index}",b"ready")
                    last_ready=now
                with lock:done=counters["received"]>=active*samples
                if not negative and done:break
            time.sleep(0.008)
        values=sorted(lat)
        p95=values[(95*len(values)+99)//100-1] if values else 0
        print(json.dumps({"role":role,"topics":topics,"bytes":size,"samples":samples,
            "sent":sent,"received":counters["received"],"bad":counters["bad"],
            "duplicate":counters["duplicate"],"p95_us":p95,"ready":int(ready),
            "index":index,"active_topics":active,"zenoh_mode":mode}),flush=True)
    if is_pub:return 0 if ready and sent==topics*samples else 3
    if negative:return 0 if counters["received"]==0 and counters["bad"]==0 else 4
    return 0 if counters["bad"]==0 and counters["duplicate"]==0 and counters["received"]<=active*samples else 5

if __name__=="__main__":
    try:sys.exit(main())
    except Exception as e:
        print("zenoh bench error: "+repr(e),file=sys.stderr,flush=True)
        raise
