#!/usr/bin/env python3
"""Run N public-API CitoQ mTLS peer namespaces, preserving logs and failure evidence."""
import argparse,importlib.util,json,os,pathlib,re,subprocess,sys,time,csv
ROOT=pathlib.Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location("m26_efficiency",ROOT/"bench/m26/m26_efficiency.py")
m26=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(m26)
def main():
 p=argparse.ArgumentParser()
 p.add_argument("--nodes",type=int,required=True)
 p.add_argument("--output",type=pathlib.Path,required=True)
 p.add_argument("--count",type=int,default=8)
 a=p.parse_args()
 a.output.mkdir(parents=True,exist_ok=True)
 handles={}
 files={}
 rows=[]
 metrics={}
 certs=ROOT/"benchmark-certs"
 env=dict(os.environ,PYTHONPATH=str(ROOT/"python"),
          CITOQ_LIBRARY=str(ROOT/"build-benchmark/libcitoq.so"),
          CITOQ_NATIVE_LIBRARY=str(ROOT/"build-benchmark/libcitoq_native.so"))
 def launch(i,role):
  fp=a.output/f"node-{i:03d}-{role}.log"
  f=fp.open("w");files[i]=f
  args=["sudo","-n","ip","netns","exec",f"m26-{i:03d}","env"]
  args +=[f"{k}={env[k]}" for k in ("PYTHONPATH","CITOQ_LIBRARY","CITOQ_NATIVE_LIBRARY")]
  args +=[sys.executable,str(ROOT/"bench/matrix/public_discovery_node.py"),role,
    "--index",str(i),"--nodes",str(a.nodes),"--count",str(a.count),
    "--ca",str(certs/"ca.pem"),
    "--server-cert",str(certs/"server.pem"),
    "--server-key",str(certs/"server.key"),
    "--client-cert",str(certs/"client.pem"),
    "--client-key",str(certs/"client.key")]
  handles[i]=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT,cwd=ROOT)
 try:
  m26.setup(a.nodes,"baseline")
  before=m26.interface_stats(a.nodes)
  for i in range(1,a.nodes):
   launch(i,"negative" if i==a.nodes-1 else "subscriber")
   time.sleep(.012)
  deadline=time.monotonic()+12
  while time.monotonic()<deadline:
   if all("RECEIVER_READY" in (a.output/f"node-{i:03d}-{'negative' if i==a.nodes-1 else 'subscriber'}.log").read_text(errors="replace") for i in range(1,a.nodes)):break
   if any(h.poll() is not None for h in handles.values()):raise RuntimeError("receiver exited before readiness")
   time.sleep(.04)
  else:raise RuntimeError("receivers did not initialize")
  prior=m26.interface_stats(a.nodes)
  launch(0,"publisher")
  sender=handles[0]
  try:sender.wait(timeout=30)
  except subprocess.TimeoutExpired:sender.kill();sender.wait()
  for i,h in handles.items():
   if i==0:continue
   try:h.wait(timeout=34)
   except subprocess.TimeoutExpired:h.kill();h.wait()
  after=m26.interface_stats(a.nodes)
  for i,h in handles.items():
   role="publisher" if i==0 else "negative" if i==a.nodes-1 else "subscriber"
   txt=(a.output/f"node-{i:03d}-{role}.log").read_text(errors="replace")
   found=re.findall(r"NODE_RESULT=(\{[^\n]+\})",txt)
   result=json.loads(found[-1]) if found else {}
   rows.append(dict(result,index=i,role=role,exit=h.returncode))
  metrics=dict(nodes=a.nodes,all_processes=len(handles)==a.nodes and all(x["exit"]==0 for x in rows),
      ready_ms=next((x.get("ready_ms") for x in rows if x["role"]=="publisher"),None),
      matched=next((x.get("matched") for x in rows if x["role"]=="publisher"),None),
      expected_peers=a.nodes-2,
      received=sum(x.get("received",0) for x in rows if x["role"]=="subscriber"),
      expected_deliveries=(a.nodes-2)*a.count,
      negative_received=sum(x.get("received",0) for x in rows if x["role"]=="negative"),
      prep_receive_veth_bytes=m26.delta(prior,before)["rx_bytes"],
      whole_trial_veth_rx_bytes=m26.delta(after,before)["rx_bytes"])
 except Exception as exc:
  metrics=dict(nodes=a.nodes,error=str(exc),all_processes=False)
 finally:
  for h in handles.values():
   if h.poll() is None:h.terminate()
  for h in handles.values():
   try:h.wait(timeout=2)
   except subprocess.TimeoutExpired:h.kill();h.wait()
  for f in files.values():f.close()
  m26.cleanup(a.nodes)
 metrics["accepted"]=bool(metrics.get("all_processes") and
    metrics.get("ready_ms") is not None and
    metrics.get("matched")==metrics.get("expected_peers") and
    metrics.get("received")==metrics.get("expected_deliveries") and
    metrics.get("negative_received")==0)
 (a.output/"results.json").write_text(json.dumps(dict(summary=metrics,peers=rows),indent=2)+"\n")
 print("PUBLIC_DISCOVERY_MATRIX="+json.dumps(metrics,sort_keys=True),flush=True)
 return 0 if metrics["accepted"] else 1
if __name__=="__main__":sys.exit(main())
