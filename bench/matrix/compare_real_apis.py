#!/usr/bin/env python3
"""Public CitoQ Context/mTLS vs public DDS/Zenoh/FastDDS APIs, without CitoQ UDP prototype."""
import argparse,csv,json,pathlib,statistics,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
MAPPING={"dds":"cyclonedds","zenoh":"zenoh","fastdds":"fastdds"}
def main():
 p=argparse.ArgumentParser()
 p.add_argument("--nodes",type=int,choices=(3,10,30),required=True)
 p.add_argument("--repeats",type=int,default=2)
 p.add_argument("--root",type=pathlib.Path,required=True)
 a=p.parse_args()
 a.root.mkdir(parents=True,exist_ok=True)
 records=[];failures=[]
 for rep in range(1,a.repeats+1):
  output=a.root/f"citoq-public-r{rep}"
  cmd=[sys.executable,str(ROOT/"bench/matrix/public_discovery.py"),"--nodes",str(a.nodes),
       "--count","4","--lifetime-ms","16000","--output",str(output)]
  with (a.root/f"citoq-public-r{rep}.log").open("w") as log:
   proc=subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=False)
  result_path=output/"results.json"
  result=json.loads(result_path.read_text())["summary"] if result_path.exists() else {"accepted":False,"error":"missing result"}
  valid=bool(proc.returncode==0 and result.get("accepted") is True and
             result.get("time_to_all_first_data_ms") is not None and
             result.get("received")==4*(a.nodes-2) and
             result.get("negative_received")==0)
  if not valid:failures.append(f"public CitoQ trial {rep}: exit={proc.returncode}, details={result}")
  records.append(dict(engine="citoq-public",nodes=a.nodes,repeat=rep,valid=valid,
    time_to_all_first_data_ms=result.get("time_to_all_first_data_ms"),
    publisher_ready_ms=result.get("ready_ms"),
    publisher_to_first_data_rx_bytes=result.get("publisher_to_first_data_rx_bytes"),
    full_veth_rx_bytes=result.get("whole_trial_veth_rx_bytes"),
    total_delivered=result.get("received"),total_expected=result.get("expected_deliveries"),
    result_path=str(result_path),error=result.get("error")))
  print("PUBLIC_API_NODE_TRIAL="+json.dumps(records[-1],sort_keys=True),flush=True)
 peerout=a.root/"other-public-apis"
 bins={"dds":ROOT/"build/bench-m26/dds_multi_topic_node",
       "zenoh":ROOT/"build/zenoh/zenoh_multi_topic_node",
       "fastdds":ROOT/"build/fastdds/fastdds_multi_topic_node"}
 cmd=[sys.executable,str(ROOT/"bench/m26/m26_efficiency.py"),
      "--engines","dds","zenoh","fastdds",
      "--nodes",str(a.nodes),"--topics","1","--percent","100","--bytes","256",
      "--samples","4","--hz","10","--repeats",str(a.repeats),
      "--stagger-ms","4","--lifetime-ms","16000",
      "--profile","baseline","--output",str(peerout)]
 for eng,pth in bins.items():cmd+=["--"+eng,str(pth)]
 with (a.root/"other-public-apis.log").open("w") as log:
  proc=subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=False)
 if proc.returncode:failures.append(f"DDS/Zenoh/FastDDS subprocess failed: {proc.returncode}")
 raw_path=peerout/"results.json"
 raw=json.loads(raw_path.read_text()) if raw_path.exists() else []
 if len(raw)!=3*a.repeats:failures.append(f"expected {3*a.repeats} peer rows, got {len(raw)}")
 for row in raw:
  valid=bool(row.get("valid") is True and row.get("all_received") is True and
    row.get("received")==4*(a.nodes-2) and row.get("negative_received")==0 and
    row.get("time_to_all_first_data_ms") is not None and
    row.get("publisher_to_first_data_rx_bytes") is not None)
  if not valid:failures.append(f"invalid peer trial {row}")
  record=dict(engine=MAPPING.get(row["engine"],row["engine"]),nodes=a.nodes,repeat=row["repeat"],
    valid=valid,time_to_all_first_data_ms=row.get("time_to_all_first_data_ms"),
    publisher_ready_ms=row.get("discovery_ready_ms"),
    publisher_to_first_data_rx_bytes=row.get("publisher_to_first_data_rx_bytes"),
    full_veth_rx_bytes=row.get("veth_rx_bytes"),
    total_delivered=row.get("received"),total_expected=row.get("expected"),
    result_path=str(peerout),error=row.get("reason"))
  records.append(record)
  print("PUBLIC_API_NODE_TRIAL="+json.dumps(record,sort_keys=True),flush=True)
 expected=len(records)==4*a.repeats
 summ=[]
 for name in ("citoq-public","cyclonedds","zenoh","fastdds"):
  cohort=[r for r in records if r["engine"]==name]
  good=[r for r in cohort if r["valid"]]
  if len(good)!=a.repeats:failures.append(f"{name}: expected {a.repeats} accepted, found {len(good)}")
  def med(key):
   vs=[r[key] for r in good if isinstance(r[key],(int,float))]
   return round(statistics.median(vs),3) if len(vs)==a.repeats else None
  summ.append(dict(engine=name,nodes=a.nodes,trials=len(cohort),
    accepted_trials=len(good),
    median_time_to_all_first_data_ms=med("time_to_all_first_data_ms"),
    median_publisher_ready_ms=med("publisher_ready_ms"),
    median_publisher_to_first_data_rx_bytes=med("publisher_to_first_data_rx_bytes"),
    median_full_veth_rx_bytes=med("full_veth_rx_bytes"),
    delivered=sum(r.get("total_delivered") or 0 for r in cohort),
    expected=sum(r.get("total_expected") or 0 for r in cohort)))
 if not expected:failures.append("not all 4 API engines have complete trial counts")
 obj={"nodes":a.nodes,"product_baseline":"08188eba73284dd1069016174ad216f5e02d2cfa",
      "citoq_product_path":"Context.publish_raw + NativeRuntime + authenticated MsQuic; NOT UDP research adapter",
      "peer_paths":{"cyclonedds":"dds_write/take","zenoh":"Publisher.put/declare_subscriber",
                    "fastdds":"DataWriter.write/take_next_sample"},
      "cohort":summ,"trials":records,"failures":failures,
      "cautions":["1 topic, 256 nominal app bytes, 4 samples per interested reader, 10Hz, 500ms warmup after publisher readiness",
        "One publisher, N-2 interested readers, one negative reader; subscribers started before publisher",
        "Time-to-first-data-to-all includes matching, handshake, first application send and 500ms deliberate warmup; NOT discovery-only",
        "Readiness semantics differ and are not comparable across products",
        "CitoQ mTLS QUIC, DDS BEST_EFFORT UDP, Zenoh TCP peer; security/reliability differ",
        "bytes sampled at 25/50ms resolution; include control and actual probe/application bytes",
        "Linux network namespaces on same kernel are not physical hosts"]}
 (a.root/"public-api-comparison.json").write_text(json.dumps(obj,indent=2)+"\n")
 with (a.root/"public-api-comparison.csv").open("w",newline="") as f:
  wr=csv.DictWriter(f,fieldnames=list(summ[0]));wr.writeheader();wr.writerows(summ)
 print("PUBLIC_API_NODE_COMPARISON="+json.dumps({"nodes":a.nodes,"cohort":summ,"failures":failures},sort_keys=True),flush=True)
 print(f"PUBLIC_API_NODE_ACCEPTED={sum(x['accepted_trials'] for x in summ)}/{4*a.repeats}",flush=True)
 for error in failures:print("PUBLIC_API_NODE_FAILURE "+error,file=sys.stderr,flush=True)
 return 0 if not failures else 1
if __name__=="__main__":sys.exit(main())
