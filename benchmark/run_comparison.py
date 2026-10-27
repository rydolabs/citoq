#!/usr/bin/env python3
"""Separate-namespace benchmark orchestrator; raw logs + CSV always retained."""
import argparse,csv,json,os,pathlib,shlex,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parents[1]; OUT=ROOT/"benchmark-evidence"
NAMES=("citoq","cyclonedds","zenoh")
NS=("citoq-ba","citoq-bb")
def cmd(*parts,check=True):
    p=subprocess.run(list(map(str,parts)),text=True,capture_output=True)
    if check and p.returncode: raise RuntimeError(f"{shlex.join(list(map(str,parts)))} exited {p.returncode}: {p.stdout} {p.stderr}")
    return p
def setup():
    for ns in NS: cmd("sudo","ip","netns","del",ns,check=False)
    cmd("sudo","ip","link","del","citoq-br",check=False)
    for ns in NS: cmd("sudo","ip","netns","add",ns)
    cmd("sudo","ip","link","add","citoq-br","type","bridge")
    cmd("sudo","ip","link","set","citoq-br","up")
    for ns,host,peer,addr in zip(NS,("citoq-ha","citoq-hb"),("citoq-pa","citoq-pb"),("10.217.0.1","10.217.0.2")):
        cmd("sudo","ip","link","add",host,"type","veth","peer","name",peer)
        cmd("sudo","ip","link","set",host,"netns",ns)
        cmd("sudo","ip","link","set",peer,"master","citoq-br")
        cmd("sudo","ip","link","set",peer,"up")
        cmd("sudo","ip","netns","exec",ns,"ip","link","set","lo","up")
        cmd("sudo","ip","netns","exec",ns,"ip","link","set",host,"name","eth0")
        cmd("sudo","ip","netns","exec",ns,"ip","addr","add",addr+"/24","dev","eth0")
        cmd("sudo","ip","netns","exec",ns,"ip","link","set","eth0","up")
    cmd("sudo","ip","netns","exec",NS[0],"ping","-c","3","-W","2","10.217.0.2")
    (OUT/"bridge.txt").write_text(cmd("ip","-d","link","show","citoq-br").stdout)
def set_profile(profile):
    for ns in NS:
        cmd("sudo","ip","netns","exec",ns,"tc","qdisc","del","dev","eth0","root",check=False)
        if profile=="netem_10ms_1pct":
            cmd("sudo","ip","netns","exec",ns,"tc","qdisc","add","dev","eth0","root","netem","delay","10ms","2ms","loss","1%")
        (OUT/f"qdisc-{profile}-{ns}.txt").write_text(cmd("sudo","ip","netns","exec",ns,"tc","-s","qdisc","show","dev","eth0").stdout)
def bytes_total():
    return {f"{name}_{direction}":int(pathlib.Path(f"/sys/class/net/{name}/statistics/{direction}_bytes").read_text())
            for name in ("citoq-pa","citoq-pb") for direction in ("rx","tx")}
def launch(engine,role,py,count,payload,rate,result,poll_mode):
    ns=NS[0] if role=="sender" else NS[1]
    env={"PYTHONPATH":str(ROOT/"python"),"CITOQ_LIBRARY":str(ROOT/"build-benchmark/libcitoq.so"),"CITOQ_NATIVE_LIBRARY":str(ROOT/"build-benchmark/libcitoq_native.so")}
    if engine=="fastdds":
        args=[str(ROOT/"build-benchmark-adapter/fastdds_node"),role,str(count),str(payload),str(rate),str(result)]
    else:
        args=[py,str(ROOT/"benchmark/bench_driver.py"),engine,role,"--count",str(count),"--payload",str(payload),
              "--rate",str(rate),"--timeout","18","--ca",str(ROOT/"benchmark-certs/ca.pem"),
              "--cert",str(ROOT/"benchmark-certs/server.pem"),"--key",str(ROOT/"benchmark-certs/server.key"),
              "--client-cert",str(ROOT/"benchmark-certs/client.pem"),"--client-key",str(ROOT/"benchmark-certs/client.key"),
              "--result",str(result)]
        if engine=="citoq":
            args += ["--citoq-poll-mode",poll_mode]
    command=["sudo","ip","netns","exec",ns,"env"]+[f"{k}={v}" for k,v in env.items()]+args
    return subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
def bench(engine,profile,rep,py,count,payload,rate,poll_mode):
    pref=f"{profile}-{engine}-r{rep}"; result=OUT/(pref+".json")
    before=bytes_total()
    receiver=launch(engine,"receiver",py,count,payload,rate,result,poll_mode)
    time.sleep(2.5)
    sender=launch(engine,"sender",py,count,payload,rate,OUT/(pref+"-sender.json"),poll_mode)
    try: sender_out,_=sender.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        sender.kill(); sender_out,_=sender.communicate()
    try: receiver_out,_=receiver.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        receiver.kill(); receiver_out,_=receiver.communicate()
    after=bytes_total()
    (OUT/(pref+"-sender.log")).write_text(sender_out)
    (OUT/(pref+"-receiver.log")).write_text(receiver_out)
    try: summary=json.loads(result.read_text())
    except Exception as exc: summary={"error":"result unavailable: "+str(exc)}
    summary.update(engine=engine,profile=profile,repetition=rep,citoq_poll_mode=poll_mode if engine=="citoq" else "not_applicable",sender_exit=sender.returncode,
                   receiver_exit=receiver.returncode,
                   bridge_rx_delta=sum(after[k]-before[k] for k in after if k.endswith("_rx")),
                   bridge_tx_delta=sum(after[k]-before[k] for k in after if k.endswith("_tx")))
    summary["accepted"]=sender.returncode==0 and receiver.returncode==0 and summary.get("complete")==True and not summary.get("error")
    (OUT/(pref+"-summary.json")).write_text(json.dumps(summary,indent=2)+"\n")
    print("BENCH_SUMMARY "+json.dumps(summary,sort_keys=True),flush=True)
    return summary
def main():
    p=argparse.ArgumentParser()
    p.add_argument("--python",default=sys.executable)
    p.add_argument("--engines",default=",".join(NAMES))
    p.add_argument("--repeats",type=int,default=2)
    p.add_argument("--count",type=int,default=200)
    p.add_argument("--payload",type=int,default=256)
    p.add_argument("--rate",type=float,default=100)
    p.add_argument("--profiles",default="baseline,netem_10ms_1pct")
    p.add_argument("--citoq-poll-mode",choices=("immediate","legacy"),default="immediate")
    p.add_argument("--output-dir",default="benchmark-evidence")
    a=p.parse_args()
    global OUT
    OUT=ROOT/a.output_dir
    OUT.mkdir(parents=True,exist_ok=True)
    ver={"source_sha":"08188eba73284dd1069016174ad216f5e02d2cfa",
         "harness_sha":cmd("git","rev-parse","HEAD").stdout.strip(),
         "kernel":cmd("uname","-r").stdout.strip(),
         "fastdds":cmd("dpkg-query","-W","libfastrtps-dev",check=False).stdout.strip(),
         "fastddsgen":cmd("dpkg-query","-W","fastddsgen",check=False).stdout.strip()}
    (OUT/"versions.json").write_text(json.dumps(ver,indent=2)+"\n")
    rows=[]
    try:
        setup()
        for profile in a.profiles.split(","):
            set_profile(profile)
            for rep in range(1,a.repeats+1):
                engines=a.engines.split(",")
                if rep%2==0: engines.reverse()
                for engine in engines:
                    try: rows.append(bench(engine,profile,rep,a.python,a.count,a.payload,a.rate,a.citoq_poll_mode))
                    except Exception as exc:
                        print(f"BENCH_FAILURE {engine}: {exc}",flush=True)
                        rows.append({"engine":engine,"profile":profile,"repetition":rep,"accepted":False,"error":str(exc)})
    finally:
        if rows:
            fields=sorted({key for row in rows for key in row})
            with (OUT/"summary.csv").open("w",newline="") as f:
                w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(rows)
            (OUT/"summary.json").write_text(json.dumps(rows,indent=2)+"\n")
        for ns in NS: cmd("sudo","ip","netns","del",ns,check=False)
        cmd("sudo","ip","link","del","citoq-br",check=False)
    good=sum(bool(x.get("accepted")) for x in rows)
    print(f"ACCEPTED={good}/{len(rows)}",flush=True)
    return 0 if rows and good==len(rows) else 1
if __name__=="__main__": sys.exit(main())
