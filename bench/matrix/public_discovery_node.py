#!/usr/bin/env python3
"""Public Context + native mTLS/QUIC multi-node readiness probe, not a cross-engine fair ranking."""
import argparse,hashlib,json,os,struct,sys,time
from contextlib import contextmanager
from pathlib import Path
import citoq
from citoq.native_runtime import NativeRuntime
TYPE=424242
RESOURCE="robot_1"
def now():return time.monotonic_ns()

# Same A/B harness supports the older native runtime without diagnostic ABI.
# No CitoQ product library changes are needed on either side.
def diagnostics(transport):
 fn=getattr(transport,"diagnostics",None)
 return fn() if fn else {}

@contextmanager
def observed_runtime(**opts):
 print("BENCH_PHASE=native_open_begin",flush=True)
 with NativeRuntime(**opts) as transport:
  print("BENCH_PHASE=native_open_done",flush=True)
  try:
   yield transport
  finally:
   print("BENCH_PHASE=native_close_begin",flush=True)
 print("BENCH_PHASE=native_close_done",flush=True)
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
 print("BENCH_SOURCE_SHA256="+hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),flush=True)
 print("BENCH_NODE_START="+json.dumps(dict(role=a.role,index=a.index,nodes=a.nodes,pid=os.getpid())),flush=True)
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
 last_heartbeat=time.monotonic()
 def heartbeat(transport,phase,**extra):
  nonlocal last_heartbeat
  current=time.monotonic()
  if current-last_heartbeat<5:return
  last_heartbeat=current
  print("BENCH_PROGRESS="+json.dumps(dict(
   role=a.role,index=a.index,phase=phase,
   uptime_ns=now(),received=len(seen),
   transport=diagnostics(transport),**extra)),flush=True)
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
    print(f"BENCH_FIRST_DATA ns={first_received_ns} transport="+json.dumps(diagnostics(transport)),flush=True)
   seen.add(seq)
   if len(seen)==a.count:
    print("BENCH_RECEIVED_ALL transport="+json.dumps(diagnostics(transport)),flush=True)
 with observed_runtime(**opts) as transport, citoq.Context("acme") as ctx:
  if not pub:
   sub_type=TYPE if a.role=="subscriber" else TYPE+10000
   ctx.subscribe_raw(sub_type,recv,RESOURCE)
   print("RECEIVER_READY",flush=True)
   until=time.monotonic()+a.lifetime_ms/1000
   while time.monotonic()<until:
    transport.poll()
    heartbeat(transport,"receiver_poll")
    if a.role=="subscriber" and len(seen)==a.count:
     # Observe a bounded grace period for duplicate deliveries.
     grace=time.monotonic()+.2
     while time.monotonic()<grace:
      transport.poll();time.sleep(.0005)
     # For the opt-in FIN-ACK gate, keep the receive-side QUIC connection
     # alive until the publisher has acknowledged all stream FINs.
     # Otherwise a fast receiver exit races with the sender ACK gate.
     # The existing lifetime deadline remains the upper bound.
     if os.environ.get("CITOQ_BENCH_WAIT_QUIC_ACK")=="1" and a.completion_file:
      marker=Path(a.completion_file)
      print("BENCH_PHASE=receiver_wait_publisher_ack",flush=True)
      while time.monotonic()<until and not marker.exists():
       transport.poll()
       heartbeat(transport,"receiver_wait_publisher_ack")
       time.sleep(.002)
      print("BENCH_PHASE=receiver_ack_wait_done marker="+str(marker.exists()),flush=True)
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
   print("BENCH_PHASE=receiver_return transport="+json.dumps(diagnostics(transport)),flush=True)
   print("NODE_RESULT="+json.dumps(dict(role=a.role,index=a.index,received=len(seen),duplicates=duplicates,invalid=invalid,first_received_ns=first_received_ns)),flush=True)
   return 0 if (a.role=="negative" and len(seen)==0 or a.role=="subscriber" and len(seen)==a.count) and duplicates==0 and invalid==0 else 2
  ctx.advertise_raw(TYPE,RESOURCE)
  t0=now()
  # Comparison mode: observe established QUIC sessions without injecting
  # probe data streams. The original publish-probe mode remains the default.
  passive_ready=os.environ.get("CITOQ_BENCH_PASSIVE_READY")=="1"
  print("BENCH_READY_MODE="+("session_count" if passive_ready else "publish_probe"),flush=True)
  target=a.nodes-2
  matched=0
  until=time.monotonic()+a.discovery_timeout_ms/1000
  probe=0
  while time.monotonic()<until:
   transport.poll()
   heartbeat(transport,"publisher_discovery",matched=matched,probes=probe)
   probe+=1
   if passive_ready:
    matched=diagnostics(transport).get("session",0)
   else:
    matched=ctx.publish_raw(TYPE,b"P"+struct.pack("<Q",probe),RESOURCE)
   transport.poll()
   if matched==target:break
   # Sparse readiness probes avoid excessive QUIC stream fanout.
   time.sleep(float(os.environ.get("CITOQ_BENCH_PROBE_INTERVAL_MS","1000"))/1000)
  ready_ms=(now()-t0)/1e6 if matched==target else None
  if ready_ms is not None:
   print(f"PUBLIC_DISCOVERY_READY elapsed_ms={ready_ms:.3f}",flush=True)
   print("BENCH_PHASE=publisher_ready probes="+str(probe)+" transport="+
         json.dumps(diagnostics(transport)),flush=True)
   warm_until=time.monotonic()+0.5
   while time.monotonic()<warm_until:
    transport.poll()
    time.sleep(.0005)
   for i in range(1,a.count+1):
    heartbeat(transport,"publisher_data",frame=i)
    sent=ctx.publish_raw(TYPE,b"D"+struct.pack("<Q",i)+b"A"*247,RESOURCE)
    transport.poll()
    print("BENCH_SEND_FRAME="+json.dumps(dict(seq=i,matched=sent,transport=diagnostics(transport))),flush=True)
    if sent!=target:
     print("PUBLIC_SEND_FAILED "+str(sent),flush=True)
     return 3
    time.sleep(.1)
   print("BENCH_PHASE=publisher_sent_all transport="+json.dumps(
    diagnostics(transport)),flush=True)
   if os.environ.get("CITOQ_BENCH_WAIT_QUIC_ACK")=="1":
    # Test-only reliability gate: keep the QUIC connections alive until
    # every data stream FIN is acknowledged, not merely buffered locally.
    expected_acks=target*a.count
    ack_deadline=time.monotonic()+min(30,a.discovery_timeout_ms/1000)
    acked=0
    while time.monotonic()<ack_deadline:
     transport.poll()
     snapshot=diagnostics(transport)
     acked=snapshot.get("acknowledged_send",0)
     if acked>=expected_acks:break
     heartbeat(transport,"publisher_wait_quic_ack",acknowledged=acked,
               expected_acks=expected_acks)
     time.sleep(.002)
    print("BENCH_ACK_STATUS="+json.dumps(dict(
     acknowledged=acked,expected=expected_acks,
     transport=diagnostics(transport))),flush=True)
    if acked<expected_acks:
     print("PUBLIC_ACK_TIMEOUT",flush=True)
     return 5
   until=time.monotonic()+1.5
   while time.monotonic()<until:
    transport.poll()
    heartbeat(transport,"publisher_drain")
    time.sleep(.0005)
  if a.completion_file:
   # Shared benchmark working directory is mounted identically in netns.
   Path(a.completion_file).write_text("finished\n")
  print("BENCH_PHASE=publisher_return transport="+json.dumps(
   diagnostics(transport)),flush=True)
  print("NODE_RESULT="+json.dumps(dict(role=a.role,index=a.index,matched=matched,expected=target,ready_ms=ready_ms)),flush=True)
  return 0 if matched==target else 4
if __name__=="__main__":
 try:sys.exit(main())
 except Exception as exc:
  import traceback
  traceback.print_exc()
  sys.exit(10)
