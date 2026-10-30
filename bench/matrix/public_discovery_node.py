#!/usr/bin/env python3
"""Public Context + native mTLS/QUIC multi-node readiness probe, not a cross-engine fair ranking."""
import argparse,json,struct,sys,time
from pathlib import Path
import citoq
from citoq.native_runtime import NativeRuntime
TYPE=424242
RESOURCE="robot_1"
def now():return time.monotonic_ns()
def main():
 p=argparse.ArgumentParser()
 p.add_argument("role",choices=("publisher","subscriber","negative"))
 p.add_argument("--index",type=int,required=True)
 p.add_argument("--nodes",type=int,required=True)
 p.add_argument("--count",type=int,default=8)
 p.add_argument("--lifetime-ms",type=int,default=16000)
 p.add_argument("--discovery-timeout-ms",type=int,default=27000)
 p.add_argument("--completion-file")
 p.add_argument("--ca",required=True)
 p.add_argument("--server-cert",required=True)
 p.add_argument("--server-key",required=True)
 p.add_argument("--client-cert",required=True)
 p.add_argument("--client-key",required=True)
 a=p.parse_args()
 pub=a.role=="publisher"
 opts=dict(coordinator=20 if pub else 50+a.index,
           incarnation=2001 if pub else 5001+a.index,
           destination=0 if pub else 77+a.index,
           control_port=47465 if pub else 47600,
           quic_port=0 if pub else 4465,
           trusted_server_ca=a.ca,
           server_cert=a.server_cert if not pub else None,
           server_key=a.server_key if not pub else None,
           client_ca=a.ca if not pub else None,
           client_cert=a.client_cert if pub else None,
           client_key=a.client_key if pub else None,
           announcement_ipv4=None if pub else "10.202.0.11",
           announcement_port=0 if pub else 47465)
 seen=set()
 first_received_ns=0
 duplicates=0
 invalid=0
 def recv(data):
  nonlocal duplicates,invalid,first_received_ns
  b=bytes(data)
  if not b or b[0]!=ord("D"):
   return
  if len(b)!=256 or b[9:]!=b"A"*247:
   invalid+=1
   return
  if len(b)<9:
   return
  seq=struct.unpack_from("<Q",b,1)[0]
  if not 1<=seq<=a.count:
   invalid+=1
  elif seq in seen:
   duplicates+=1
  else:
   if first_received_ns==0:
    first_received_ns=now()
    print(f"BENCH_FIRST_DATA ns={first_received_ns}",flush=True)
   seen.add(seq)
 with NativeRuntime(**opts) as transport, citoq.Context("acme") as ctx:
  if not pub:
   sub_type=TYPE if a.role=="subscriber" else TYPE+10000
   ctx.subscribe_raw(sub_type,recv,RESOURCE)
   print("RECEIVER_READY",flush=True)
   until=time.monotonic()+a.lifetime_ms/1000
   while time.monotonic()<until:
    transport.poll()
    if a.role=="subscriber" and len(seen)==a.count:
     # Observe a bounded grace period for duplicate deliveries.
     grace=time.monotonic()+.2
     while time.monotonic()<grace:
      transport.poll();time.sleep(.0005)
     break
    if a.role=="negative" and a.completion_file and Path(a.completion_file).exists():
     # The publisher has finished. Keep watching an additional 300ms for
     # incorrect late delivery to unrelated negative interests.
     grace=time.monotonic()+.3
     while time.monotonic()<grace:
      transport.poll();time.sleep(.0005)
     break
    if a.role=="negative" and time.monotonic()>until-2:break
    time.sleep(.0005)
   print("NODE_RESULT="+json.dumps(dict(role=a.role,index=a.index,received=len(seen),duplicates=duplicates,invalid=invalid,first_received_ns=first_received_ns)),flush=True)
   return 0 if (a.role=="negative" and len(seen)==0 or a.role=="subscriber" and len(seen)==a.count) and duplicates==0 and invalid==0 else 2
  ctx.advertise_raw(TYPE,RESOURCE)
  t0=now()
  target=a.nodes-2
  matched=0
  until=time.monotonic()+a.discovery_timeout_ms/1000
  checks=0
  while time.monotonic()<until:
   transport.poll()
   checks+=1
   matched=ctx.remote_match_count_raw(TYPE,RESOURCE)
   if matched==target:break
   time.sleep(.025)
  print(f"PUBLIC_PASSIVE_MATCH_CHECKS count={checks}",flush=True)
  ready_ms=(now()-t0)/1e6 if matched==target else None
  quic_connected=0
  quic_ready_ms=None
  if ready_ms is not None:
   print(f"PUBLIC_DISCOVERY_READY elapsed_ms={ready_ms:.3f}",flush=True)
   # A resolved DemandKey is not a connected QUIC session. This explicit
   # barrier removes the fixed 500ms assumption from delivery benchmarking.
   session_deadline=time.monotonic()+(30 if a.nodes>=200 else 15)
   while time.monotonic()<session_deadline:
    transport.poll()
    quic_connected=transport.session_count()
    if quic_connected==target:break
    time.sleep(.002)
   if quic_connected!=target:
    print(f"PUBLIC_QUIC_NOT_READY connected={quic_connected} expected={target}",flush=True)
    return 5
   quic_ready_ms=(now()-t0)/1e6
   print(f"PUBLIC_QUIC_READY elapsed_ms={quic_ready_ms:.3f}",flush=True)
   for i in range(1,a.count+1):
    sent=ctx.publish_raw(TYPE,b"D"+struct.pack("<Q",i)+b"A"*247,RESOURCE)
    transport.poll()
    if sent!=target:
     print("PUBLIC_SEND_FAILED "+str(sent),flush=True)
     return 3
    time.sleep(.1)
   until=time.monotonic()+1.5
   while time.monotonic()<until:
    transport.poll();time.sleep(.0005)
  if a.completion_file:
   # Shared benchmark working directory is mounted identically in netns.
   Path(a.completion_file).write_text("finished\n")
  print("NODE_RESULT="+json.dumps(dict(role=a.role,index=a.index,matched=matched,expected=target,ready_ms=ready_ms,quic_ready_ms=quic_ready_ms,quic_connected=quic_connected)),flush=True)
  return 0 if matched==target else 4
if __name__=="__main__":
 try:sys.exit(main())
 except Exception as exc:
  import traceback
  traceback.print_exc()
  sys.exit(10)
