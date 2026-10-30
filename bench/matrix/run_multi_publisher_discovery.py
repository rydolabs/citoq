#!/usr/bin/env python3
"""Strict N-node P-publisher all-interested subscriber Discovery scale matrix.

N=30 P=3 S=26, N=50 P=5 S=44, N=100 P=10 S=89, plus one
uninterested negative receiver. One process/netns/veth per node, same
Linux bridge + verified egress tc netem for each engine trial.

Every publisher waits for S exact CitoQ/DDS matches, or N-1 actual
Zenoh direct peer links. All P publishers synchronize at the all-ready
barrier, then simultaneously start delivering eight unique 256-byte
frames each. Every subscriber must receive the complete P*8 matrix.
"""
import argparse
import csv
import json
import pathlib
import re
import subprocess
import sys
import time

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"bench/matrix"))
import three_engine_discovery_scale as prior  # noqa: E402

net=prior.net
PROFILES=prior.PROFILES
LAYOUT={30:3,50:5,100:10}


def role_for(index, nodes, publishers):
    if index<publishers:
        return "publisher"
    if index==nodes-1:
        return "negative"
    return "subscriber"


def read_log(path):
    return path.read_text(errors="replace") if path.exists() else ""


def launch(index, nodes, publishers, engine, count, root, python,
           lifetime_ms, discovery_ms, processes, streams):
    role=role_for(index,nodes,publishers)
    log=root/f"node-{index:03d}-{role}.log"
    stream=log.open("w")
    streams.append(stream)
    resource=root/f"node-{index:03d}-{role}.resource"
    ns=f"m26-{index:03d}"
    env=[
        "PYTHONPATH="+str(ROOT/"python"),
        "CITOQ_LIBRARY="+str(ROOT/"build-benchmark/libcitoq.so"),
        "CITOQ_NATIVE_LIBRARY="+str(ROOT/"build-benchmark/libcitoq_native.so")
    ]
    if engine=="cyclonedds":
        config=('<CycloneDDS><Domain id="any"><General><Interfaces>'
                '<NetworkInterface name="eth0"/></Interfaces>'
                '<AllowMulticast>spdp</AllowMulticast>'
                '</General></Domain></CycloneDDS>')
        env.append("CYCLONEDDS_URI="+config)
    cmd=[
        "sudo","-n","ip","netns","exec",ns,"env",*env,
        "/usr/bin/time","-f","user_s=%U system_s=%S max_rss_kb=%M",
        "-o",str(resource.resolve()),
        str(python),str(ROOT/"bench/matrix/multi_publisher_discovery_node.py"),
        role,"--engine",engine,"--index",str(index),
        "--nodes",str(nodes),"--publishers",str(publishers),
        "--subscribers",str(nodes-publishers-1),"--count",str(count),
        "--lifetime-ms",str(lifetime_ms),
        "--discovery-timeout-ms",str(discovery_ms),
        "--output",str(root.resolve()),
    ]
    if engine=="citoq":
        certs=ROOT/"benchmark-certs"
        for flag,path in [
            ("ca",certs/"ca.pem"),
            ("server-cert",certs/"server.pem"),
            ("server-key",certs/"server.key"),
            ("client-cert",certs/"client.pem"),
            ("client-key",certs/"client.key"),
        ]:
            cmd+=["--"+flag,str(path)]
    processes[index]=subprocess.Popen(
        cmd,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,
        start_new_session=True)
    return log


def metrics_for_node(root,index,nodes,publishers,proc):
    role=role_for(index,nodes,publishers)
    file=root/f"node-{index:03d}-{role}.log"
    result=prior.parse_node(file)
    resource=root/f"node-{index:03d}-{role}.resource"
    text=resource.read_text(errors="replace") if resource.exists() else ""
    m=re.search(
        r"user_s=([0-9.]+) system_s=([0-9.]+) max_rss_kb=(\d+)",
        text)
    return {**result, "index":index,"role":role,"exit":proc.returncode,
            "resource":(
                dict(user_s=float(m[1]),system_s=float(m[2]),
                     rss_kb=int(m[3]))
                if m else None)}


def trial(nodes,publishers,engine,profile,repetition,count,
          root,python,lifetime_ms,discovery_ms):
    folder=root/f"{engine}-n{nodes}-p{publishers}-{profile}-r{repetition}"
    folder.mkdir(parents=True,exist_ok=True)
    streams=[]
    processes={}
    rows=[]
    before=at_receiver_ready=at_publish_ready=after=None
    s=nodes-publishers-1
    expected=publishers*s*count
    summary=dict(
        engine=engine,nodes=nodes,publishers=publishers,subscribers=s,
        negative_subscribers=1,profile=profile,repetition=repetition,
        count=count,expected_deliveries=expected,received=0,
        message_bytes=256,
        topology="P publishers each has separate topic; S subscribers interested in all P; one negative node",
        transport=("MsQuic/mTLS" if engine=="citoq" else
                   "peer TCP multicast scouting+gossip (NO TLS)" if engine=="zenoh"
                   else "DDS/RTPS reliable UDP SPDP+SEDP (NO TLS)"),
        accepted=False,
    )
    run_start=time.monotonic()
    publisher_start_ns=0
    try:
        net.setup(nodes,"baseline")
        prior.apply_profile(nodes,profile,folder)
        before=net.interface_stats(nodes)
        receiver_logs=[]
        for i in range(publishers,nodes):
            receiver_logs.append(
                launch(i,nodes,publishers,engine,count,folder,python,
                       lifetime_ms,discovery_ms,processes,streams))
            time.sleep(.012)
        readiness_deadline=time.monotonic()+75
        while time.monotonic()<readiness_deadline:
            if any(p.poll() is not None for p in processes.values()):
                raise RuntimeError("one receiver exited before RECEIVER_READY")
            if all("RECEIVER_READY" in read_log(path)
                   for path in receiver_logs):
                break
            time.sleep(.10)
        else:
            raise RuntimeError("not all receivers reached RECEIVER_READY")
        summary["receivers_ready"]=len(receiver_logs)
        at_receiver_ready=net.interface_stats(nodes)
        publisher_start_ns=time.monotonic_ns()
        publisher_logs=[]
        for i in range(publishers):
            publisher_logs.append(
                launch(i,nodes,publishers,engine,count,folder,python,
                       lifetime_ms,discovery_ms,processes,streams))
            time.sleep(.02)
        # The gate is *all* P publishers having discovered all S targets.
        # Capture bytes before any publisher starts sending application data.
        deadline=time.monotonic()+min(140,(discovery_ms+count*120+20000)/1000)
        saw_ready=False
        while time.monotonic()<deadline:
            all_ready=all(
                (folder/f"pub-{p:03d}-ready.marker").exists()
                for p in range(publishers))
            if not saw_ready and all_ready:
                at_publish_ready=net.interface_stats(nodes)
                saw_ready=True
            if all(p.poll() is not None for p in processes.values()):
                break
            time.sleep(.10)
        else:
            summary["timeout"]=True
            summary["alive_indexes"]=[
                i for i,p in processes.items() if p.poll() is None]
        after=net.interface_stats(nodes)
    except Exception as exc:
        summary["error"]=str(exc)
        try:
            after=net.interface_stats(nodes)
        except Exception:
            pass
    finally:
        # Capture the *partial* state even when timeout / no NODE_RESULT.
        for i,p in processes.items():
            p.poll()
        for i,p in sorted(processes.items()):
            rows.append(metrics_for_node(folder,i,nodes,publishers,p))
        writers=[r for r in rows if r["role"]=="publisher"]
        readers=[r for r in rows if r["role"]=="subscriber"]
        negative=[r for r in rows if r["role"]=="negative"]
        summary["publisher_ready_count"]=sum(
            (folder/f"pub-{i:03d}-ready.marker").exists()
            for i in range(publishers))
        summary["publisher_done_count"]=sum(
            (folder/f"pub-{i:03d}-done.marker").exists()
            for i in range(publishers))
        summary["publisher_matches"]={
            str(r["index"]):r.get("matched") for r in writers}
        summary["zenoh_publisher_links"]={
            str(r["index"]):r.get("links") for r in writers}
        summary["publisher_sent"]={
            str(r["index"]):r.get("sent") for r in writers}
        summary["received"]=sum(r.get("received",0) for r in readers)
        summary["duplicates"]=sum(r.get("duplicates",0) for r in rows)
        summary["invalid"]=sum(r.get("invalid",0) for r in rows)
        summary["negative_received"]=sum(r.get("received",0)
                                          for r in negative)
        summary["subscriber_incomplete"]=[
            dict(index=r["index"],received=r.get("received"),
                 missing_by_publisher=r.get("missing_by_publisher"))
            for r in readers if r.get("received")!=publishers*count]
        summary["exits_ok"]=(
            len(rows)==nodes and all(r["exit"]==0 for r in rows))
        all_first=[]
        for r in readers:
            times=r.get("first_by_pub_ns",{})
            if len(times)==publishers:
                all_first.extend(times.values())
        summary["all_edge_first_ms"]=(
            round((max(all_first)-publisher_start_ns)/1e6,3)
            if publisher_start_ns and len(all_first)==s*publishers
            and all(t>publisher_start_ns for t in all_first)
            else None)
        ready_at=[r.get("ready_ms") for r in writers]
        summary["publisher_ready_ms_max"]=(
            round(max(ready_at),3)
            if len(ready_at)==publishers and all(t is not None for t in ready_at)
            else None)
        resources=[r["resource"] for r in rows if r["resource"]]
        summary["resource_rows"]=len(resources)
        summary["cpu_total_s"]=round(sum(
            r["user_s"]+r["system_s"] for r in resources),3)
        summary["rss_kb_sum_process_max"]=sum(r["rss_kb"] for r in resources)
        if before and at_receiver_ready:
            summary["receiver_startup_rx_bytes"]=net.delta(
                at_receiver_ready,before)["rx_bytes"]
        if at_receiver_ready and at_publish_ready:
            summary["publisher_discovery_rx_bytes"]=net.delta(
                at_publish_ready,at_receiver_ready)["rx_bytes"]
        if before and after:
            summary["whole_trial_rx_bytes"]=net.delta(
                after,before)["rx_bytes"]
        summary["accepted"]=bool(
            len(rows)==nodes and summary["exits_ok"] and
            summary["publisher_ready_count"]==publishers and
            summary["publisher_done_count"]==publishers and
            summary["resource_rows"]==nodes and
            summary["all_edge_first_ms"] is not None and
            summary["received"]==expected and
            not summary["subscriber_incomplete"] and
            summary["negative_received"]==0 and
            summary["duplicates"]==0 and summary["invalid"]==0 and
            all(r.get("sent")==count for r in writers) and
            all((r.get("matched")==s if engine!="zenoh"
                 else r.get("links",0)>=nodes-1) for r in writers) and
            not summary.get("error") and not summary.get("timeout"))
        summary["elapsed_s"]=round(time.monotonic()-run_start,3)
        # Stop actual workers, not sudo wrapper processes.
        prior.kill_workers(nodes)
        for proc in processes.values():
            if proc.poll() is None:proc.terminate()
        for proc in processes.values():
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for stream in streams:stream.close()
        try:net.cleanup(nodes)
        except Exception as exc:
            summary["cleanup_error"]=str(exc)
            summary["accepted"]=False
        (folder/"trial.json").write_text(
            json.dumps(dict(summary=summary,nodes=rows,
                            before=before,at_receiver_ready=at_receiver_ready,
                            at_publisher_ready=at_publish_ready,after=after),
                       indent=2)+"\n")
        print("MULTIPUB_DISCOVERY_TRIAL="+json.dumps(summary,sort_keys=True),
              flush=True)
    return summary


def main():
    p=argparse.ArgumentParser()
    p.add_argument("--nodes",type=int,choices=tuple(LAYOUT),required=True)
    p.add_argument("--publishers",type=int,required=True)
    p.add_argument("--engine",choices=("citoq","zenoh","cyclonedds"),required=True)
    p.add_argument("--profile",choices=tuple(PROFILES),required=True)
    p.add_argument("--repeats",type=int,default=1)
    p.add_argument("--count",type=int,default=8)
    p.add_argument("--lifetime-ms",type=int,default=135000)
    p.add_argument("--discovery-timeout-ms",type=int,default=90000)
    p.add_argument("--python",type=pathlib.Path,default=pathlib.Path(sys.executable))
    p.add_argument("--output",type=pathlib.Path,required=True)
    args=p.parse_args()
    if (args.publishers!=LAYOUT[args.nodes] or
            not 1<=args.repeats<=2 or not 1<=args.count<=32):
        p.error("invalid controlled multipublisher test layout")
    args.output.mkdir(parents=True,exist_ok=True)
    trials=[]
    for repetition in range(1,args.repeats+1):
        trials.append(trial(args.nodes,args.publishers,args.engine,
                            args.profile,repetition,args.count,args.output,
                            args.python,args.lifetime_ms,
                            args.discovery_timeout_ms))
    failures=[r for r in trials if not r["accepted"]]
    result=dict(nodes=args.nodes,publishers=args.publishers,
                subscribers=args.nodes-args.publishers-1,
                engine=args.engine,profile=args.profile,
                attempted=len(trials),accepted=len(trials)-len(failures),
                trial_summaries=trials,passed=not failures,
                limitations=[
                    "One hosted runner with N netns/veth; not N separate machines.",
                    "P distinct publishers/topics, S all-interested subscribers, 1 negative node: NOT an all-to-all publishers+subscribers mesh.",
                    "Only CitoQ uses authenticated MsQuic/mTLS; Zenoh peer TCP and CycloneDDS reliable UDP do not use TLS here.",
                    "CitoQ UDP interest summaries broadcast to 10.202.0.255 to discover all P publishers; previous one-publisher run targeted 10.202.0.11 unicast.",
                    "Zenoh transport readiness counts N-1 direct peer IDs, including uninterested nodes; CitoQ/DDS count S topic-matched readers, not equivalent.",
                    "Control wire bytes are observed only until all P publishers are ready; full traffic includes teardown and 256-byte data.",
                    "Each publisher sends eight 256-byte samples; this is a Discovery/concurrent-fanout smoke test, not sustained workload or an SLA.",
                ])
    (args.output/"summary.json").write_text(json.dumps(result,indent=2)+"\n")
    with (args.output/"summary.csv").open("w",newline="") as f:
        keys=["engine","nodes","publishers","subscribers","profile",
              "repetition","accepted","publisher_ready_ms_max",
              "all_edge_first_ms","publisher_discovery_rx_bytes",
              "whole_trial_rx_bytes","cpu_total_s","rss_kb_sum_process_max",
              "publisher_ready_count","publisher_done_count","received",
              "expected_deliveries","duplicates","invalid","negative_received",
              "elapsed_s","error","timeout"]
        writer=csv.DictWriter(f,fieldnames=keys,extrasaction="ignore")
        writer.writeheader()
        writer.writerows(trials)
    print("MULTIPUB_DISCOVERY_SUMMARY="+json.dumps(
        {k:v for k,v in result.items() if k!="trial_summaries"},
        sort_keys=True),flush=True)
    return int(bool(failures))


if __name__=="__main__":
    sys.exit(main())
