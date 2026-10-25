#!/usr/bin/env python3
"""CitoQ experimental UDP vs Cyclone DDS Linux netns: selective pub/sub byte, latency, CPU/RSS A/B.
Each scenario runs separate fresh namespaces for Cito and Cyclone DDS.
Traffic stats count actual veth bridge ingress/egress incl. broadcast copies.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time

BR = 'm26br'
PROFILES = {'baseline':(0,0,0),'limited':(1000,50,1)}
IFACE_PREFIX='m26h'
NS_PREFIX='m26-'

def cmd(*args,check=True):
    p=subprocess.run(['sudo','-n',*args],text=True,capture_output=True)
    if check and p.returncode:
        raise RuntimeError(f'command {args!r}: {p.returncode}: {p.stderr[-800:]}')
    return p

def cleanup(n):
    for i in range(n):
        ns=f'{NS_PREFIX}{i:03d}'
        p=cmd('ip','netns','pids',ns,check=False)
        for pid in p.stdout.split():
            cmd('kill','-TERM',pid,check=False)
        cmd('ip','netns','del',ns,check=False)
        cmd('ip','link','del',f'{IFACE_PREFIX}{i:03d}',check=False)
    cmd('ip','link','del',BR,check=False)

def setup(n,profile):
    rate,delay,loss=PROFILES[profile]
    cleanup(n)
    cmd('ip','link','add',BR,'type','bridge')
    cmd('ip','link','set','dev',BR,'type','bridge','mcast_snooping','0')
    cmd('ip','addr','add','10.202.0.1/24','dev',BR)
    cmd('ip','link','set',BR,'up')
    for i in range(n):
        ns=f'{NS_PREFIX}{i:03d}'
        host=f'{IFACE_PREFIX}{i:03d}'
        peer=f'm26p{i:03d}'
        cmd('ip','netns','add',ns)
        cmd('ip','link','add',host,'type','veth','peer','name',peer)
        cmd('ip','link','set',host,'master',BR)
        cmd('ip','link','set',host,'up')
        cmd('ip','link','set',peer,'netns',ns)
        cmd('ip','netns','exec',ns,'ip','link','set','lo','up')
        cmd('ip','netns','exec',ns,'ip','link','set',peer,'name','eth0')
        cmd('ip','netns','exec',ns,'ip','addr','add',f'10.202.0.{i+11}/24','dev','eth0')
        cmd('ip','netns','exec',ns,'ip','link','set','eth0','up')
        if profile!='baseline':
            opts=['limit','300']
            if delay:opts+=['delay',f'{delay/2:g}ms']
            if loss:opts+=['loss',f'{100*(1-math.sqrt(1-loss/100)):.6f}%']
            if rate:opts+=['rate',f'{rate}kbit']
            for q in [('tc','qdisc','replace','dev',host,'root','netem',*opts),
                      ('ip','netns','exec',ns,'tc','qdisc','replace','dev','eth0','root','netem',*opts)]:
                cmd(*q)
            for q in [('tc','qdisc','show','dev',host),
                      ('ip','netns','exec',ns,'tc','qdisc','show','dev','eth0')]:
                if 'netem' not in cmd(*q).stdout:
                    raise RuntimeError(f'netem missing on {q!r}')

def interface_stats(n):
    payload=json.loads(cmd('ip','-j','-s','link','show').stdout)
    interfaces={a['ifname'].split('@')[0]:a for a in payload}
    rows=[]
    for i in range(n):
        a=interfaces.get(f'{IFACE_PREFIX}{i:03d}')
        if a is None:raise RuntimeError(f'missing host veth {i}')
        stats=a.get('stats64') or a.get('stats')
        rows.append({key:stats[d][field] for d in ('rx','tx')
                     for field,key in [('bytes',d+'_bytes'),('packets',d+'_packets')]})
    return {key:sum(r[key] for r in rows) for key in rows[0]}

def delta(after,before):
    return {k:after[k]-before[k] for k in before}

def parse_metrics(path):
    if not path.exists():return None
    txt=path.read_text(errors='replace')
    hits=re.findall(r'\{[^\n]*"role"[^\n]*\}',txt)
    if not hits:return None
    return json.loads(hits[-1])

def parse_resource(path):
    if not path.exists():return {}
    s=path.read_text(errors='replace')
    m=re.search(r'user_s=([0-9.]+) system_s=([0-9.]+) max_rss_kb=(\d+)',s)
    return dict(user_s=float(m[1]),system_s=float(m[2]),max_rss_kb=int(m[3])) if m else {}

def trial(args,engine,repeat):
    n=args.nodes
    active=max(1,(args.topics*args.percent+99)//100)
    folder=args.output/f'{args.profile}-n{n}-t{args.topics}-p{args.percent}-{engine}-r{repeat}'
    folder.mkdir(parents=True,exist_ok=True)
    p_by_index={}
    files=[]
    seen={}
    before=discovery=after=None
    ready_ms=None
    reason=''
    started=time.monotonic()
    try:
        setup(n,args.profile)
        before=interface_stats(n)
        env=dict(os.environ)
        config='<CycloneDDS><Domain id="any"><General><Interfaces><NetworkInterface name="eth0"/></Interfaces><AllowMulticast>spdp</AllowMulticast></General></Domain></CycloneDDS>'
        exe=(args.cito if engine=='cito' else args.dds).resolve()
        def launch(i):
            role='publisher' if i==0 else ('negative' if i==n-1 else 'subscriber')
            log=folder/f'node-{i:03d}-{role}.log'
            file=log.open('w')
            files.append(file)
            resource=folder/f'node-{i:03d}-{role}.resource'
            call=['sudo','-n','ip','netns','exec',f'{NS_PREFIX}{i:03d}']
            if engine=='dds':
                call+=['env','CYCLONEDDS_URI='+config]
                if os.environ.get('M26_BUNDLE_LIB'):
                    call.append('LD_LIBRARY_PATH='+os.environ['M26_BUNDLE_LIB'])
            call+=['/usr/bin/time','-f','user_s=%U system_s=%S max_rss_kb=%M','-o',str(resource.resolve()),str(exe),
                   role,str(args.topics),str(args.bytes),str(args.samples),str(args.hz),'10.202.0.255',
                   str(args.lifetime_ms),str(i),str(n),str(args.percent)]
            p=subprocess.Popen(call,stdout=file,stderr=subprocess.STDOUT,env=env)
            p_by_index[i]=p
        for i in range(1,n):
            launch(i)
            if args.stagger_ms:time.sleep(args.stagger_ms/1000)
        time.sleep(.4)
        publisher_start=time.monotonic()
        launch(0)
        deadline=time.monotonic()+args.lifetime_ms/1000+12
        publisher_log=folder/'node-000-publisher.log'
        while time.monotonic()<deadline:
            if ready_ms is None and publisher_log.exists():
                txt=publisher_log.read_text(errors='replace')
                match=re.search(r'BENCH_DISCOVERY_READY elapsed_ms=(\d+)',txt)
                if match:
                    ready_ms=int(match[1])
                    discovery=interface_stats(n)
            if all(p.poll() is not None for p in p_by_index.values()):break
            time.sleep(.05)
        else:
            reason='process timeout'
        after=interface_stats(n)
    except Exception as e:
        reason='setup/execute: '+str(e)
        try:after=interface_stats(n)
        except Exception:pass
    finally:
        for p in p_by_index.values():
            if p.poll() is None:p.terminate()
        for p in p_by_index.values():
            try:p.wait(timeout=2)
            except subprocess.TimeoutExpired:p.kill();p.wait()
        for f in files:f.close()
        cleanup(n)

    publisher=parse_metrics(folder/'node-000-publisher.log') or {}
    positives=[parse_metrics(folder/f'node-{i:03d}-subscriber.log') or {} for i in range(1,n-1)]
    negative=parse_metrics(folder/f'node-{n-1:03d}-negative.log') or {}
    metrics=[parse_resource(folder/f"node-{i:03d}-{'publisher' if i==0 else ('negative' if i==n-1 else 'subscriber')}.resource") for i in range(n)]
    expected=active*args.samples*(n-2)
    received=sum(x.get('received',0) for x in positives)
    sends=publisher.get('sent',0)
    counts=[x.get('received',0) for x in positives]
    bad=sum(x.get('bad',0)+x.get('duplicate',0) for x in positives)
    all_exited=len(p_by_index)==n and all(p.returncode==0 for p in p_by_index.values())
    truth=all_exited and sends==args.topics*args.samples and bad==0 and negative.get('received',-1)==0 and negative.get('bad',-1)==0 and received<=expected and ready_ms is not None and not reason
    if engine=='cito' and negative.get('data_datagrams',-1)!=0:truth=False
    row=dict(engine=engine,repeat=repeat,profile=args.profile,nodes=n,topics=args.topics,percent=args.percent,
             subscribed_topics=active,bytes=args.bytes,hz=args.hz,samples=args.samples,
             publisher_sent=sends,expected=expected,received=received,delivery_rate=round(received/expected,6),
             all_received=received==expected,valid=truth,reason=reason,discovery_ready_ms=ready_ms,
             subscriber_p95_max_us=max((v.get('p95_us',0) for v in positives),default=0),
             positive_min_received=min(counts,default=0),
             negative_received=negative.get('received'),unrelated_data_datagrams=negative.get('data_datagrams'),
             cpu_user_s=round(sum(v.get('user_s',0) for v in metrics),4),
             cpu_system_s=round(sum(v.get('system_s',0) for v in metrics),4),
             sum_max_rss_kb=sum(v.get('max_rss_kb',0) for v in metrics),
             elapsed_ms=round((time.monotonic()-started)*1000))
    if before and after:
        wire=delta(after,before)
        row.update({f'veth_{k}':v for k,v in wire.items()})
        if discovery:
            pre=delta(discovery,before)
            row.update({f'prepublish_{k}':v for k,v in pre.items()})
    (folder/'result.json').write_text(json.dumps(dict(row=row,publisher=publisher,negative=negative,subscribers=positives,
                    resource_stats=metrics,interface_before=before,interface_prepublish=discovery,interface_after=after),indent=2)+'\n')
    return row

def main():
    p=argparse.ArgumentParser()
    for name in ('cito','dds'):p.add_argument('--'+name,required=True,type=Path)
    p.add_argument('--nodes',type=int,default=10)
    p.add_argument('--topics',type=int,default=33)
    p.add_argument('--percent',type=int,default=10)
    p.add_argument('--bytes',type=int,default=256)
    p.add_argument('--samples',type=int,default=12)
    p.add_argument('--hz',type=int,default=10)
    p.add_argument('--repeats',type=int,default=5)
    p.add_argument('--profile',choices=PROFILES,default='baseline')
    p.add_argument('--lifetime-ms',type=int,default=11000)
    p.add_argument('--stagger-ms',type=int,default=4)
    p.add_argument('--output',type=Path,default=Path('bench-results'))
    args=p.parse_args()
    if not (3<=args.nodes<=120 and 1<=args.topics<=256 and 1<=args.percent<=100
            and 12<=args.bytes<=60000 and 1<=args.repeats<=10 and args.samples>=1
            and args.hz>=1 and args.cito.is_file() and args.dds.is_file()):
        p.error('invalid shape, paylod size, repeat, or binary')
    args.output.mkdir(parents=True,exist_ok=True)
    rows=[]
    for rep in range(1,args.repeats+1):
        for engine in ('cito','dds') if rep%2 else ('dds','cito'):
            print(f'::group::M26 {engine} nodes={args.nodes} topics={args.topics} subscribed_pct={args.percent} rep={rep}',flush=True)
            row=trial(args,engine,rep)
            rows.append(row)
            print(json.dumps(row,sort_keys=True),flush=True)
            (args.output/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
            keys=sorted({k for row in rows for k in row})
            with (args.output/'summary.csv').open('w',newline='') as f:
                wr=csv.DictWriter(f,fieldnames=keys);wr.writeheader();wr.writerows(rows)
            print('::endgroup::',flush=True)
    return int(any(not x['valid'] for x in rows))
if __name__=='__main__':sys.exit(main())
