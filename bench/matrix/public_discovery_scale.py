#!/usr/bin/env python3
"""Strict public CitoQ QUIC/mTLS Discovery scale test over real Linux netns/veth/bridge.

Each trial launches N separate process/network namespaces: one publisher,
N-2 positive subscribers, and one negative-interest subscriber. Deliberately
NOT an all-to-all mesh; preserve that topology in the report.
"""
import argparse
import csv
import json
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bench/m26"))
import m26_efficiency as net  # noqa: E402

PROFILES = {"baseline": None, "netem_10ms_1pct": "1%", "netem_10ms_2pct": "2%"}
LIMIT = 100


def apply_profile(nodes, profile, output):
    loss = PROFILES[profile]
    evidence = output / "network"
    evidence.mkdir(exist_ok=True, parents=True)
    for i in range(nodes):
        namespace = f"m26-{i:03d}"
        # Both publishers and subscribers have namespace egress shaping;
        # client->server and server->client directions are independently
        # shaped without mistakenly applying two netem stages per direction.
        if loss:
            net.cmd("ip", "netns", "exec", namespace, "tc", "qdisc", "replace",
                    "dev", "eth0", "root", "netem", "delay", "10ms", "2ms",
                    "loss", loss)
        actual = net.cmd("ip", "netns", "exec", namespace, "tc", "-s",
                         "qdisc", "show", "dev", "eth0").stdout
        (evidence / f"{namespace}-qdisc.txt").write_text(actual)
        if loss:
            if "qdisc netem" not in actual or not re.search(
                r"\bloss\s+" + re.escape(loss[:-1]) + r"(?:\.0+)?%", actual
            ):
                raise RuntimeError(
                    f"{namespace}: requested {profile}, observed {actual}")
        elif "qdisc netem" in actual:
            raise RuntimeError(
                f"{namespace}: baseline unexpectedly has netem: {actual}")


def namespace_pids(ns):
    proc = net.cmd("ip", "netns", "pids", ns, check=False)
    if proc.returncode:
        return []
    return [int(x) for x in proc.stdout.split() if x.isdigit()]


def kill_workers(nodes):
    for i in range(nodes):
        ns = f"m26-{i:03d}"
        for pid in namespace_pids(ns):
            net.cmd("kill", "-KILL", str(pid), check=False)


def count_markers(log):
    if not log.exists():
        return ""
    return log.read_text(errors="replace")


def launch(index, role, nodes, count, lifetime_ms, discovery_ms,
           output, files, processes):
    ns = f"m26-{index:03d}"
    log = output / f"node-{index:03d}-{role}.log"
    stream = log.open("w")
    files.append(stream)
    certs = ROOT / "benchmark-certs"
    env = {
        "PYTHONPATH": str(ROOT / "python"),
        "CITOQ_LIBRARY": str(ROOT / "build-benchmark/libcitoq.so"),
        "CITOQ_NATIVE_LIBRARY": str(ROOT / "build-benchmark/libcitoq_native.so"),
    }
    args = [
        "sudo", "-n", "ip", "netns", "exec", ns, "env",
        *(f"{key}={value}" for key, value in env.items()),
        sys.executable, str(ROOT / "bench/matrix/public_discovery_node.py"),
        role, "--index", str(index), "--nodes", str(nodes),
        "--count", str(count), "--lifetime-ms", str(lifetime_ms),
        "--discovery-timeout-ms", str(discovery_ms),
        "--completion-file", str((output / "publisher-finished.marker").resolve()),
        "--ca", str(certs / "ca.pem"),
        "--server-cert", str(certs / "server.pem"),
        "--server-key", str(certs / "server.key"),
        "--client-cert", str(certs / "client.pem"),
        "--client-key", str(certs / "client.key"),
    ]
    processes[index] = subprocess.Popen(
        args, stdout=stream, stderr=subprocess.STDOUT,
        cwd=ROOT, start_new_session=True)
    return log


def parse_node(log):
    data = count_markers(log)
    found = re.findall(r"^NODE_RESULT=(\{[^\n]+\})$", data, re.MULTILINE)
    return json.loads(found[-1]) if len(found) == 1 else {}


def single_trial(nodes, profile, repetition, count, root, lifetime_ms, discovery_ms):
    folder = root / f"n{nodes}-{profile}-r{repetition}"
    folder.mkdir(parents=True, exist_ok=True)
    files = []
    processes = {}
    network_before = network_ready = network_first = network_after = None
    reports = []
    metrics = {
        "nodes": nodes, "profile": profile, "repetition": repetition,
        "positive_subscribers": nodes-2, "negative_subscribers": 1,
        "messages_per_positive_subscriber": count,
        "topology": "one publisher; N-2 subscribers; one negative-interest node",
        "runner": "Linux netns + veth + Linux Bridge",
        "accepted": False,
    }
    start = time.monotonic()
    try:
        net.setup(nodes, "baseline")
        apply_profile(nodes, profile, folder)
        network_before = net.interface_stats(nodes)
        receiver_logs = []
        for index in range(1, nodes):
            role = "negative" if index == nodes-1 else "subscriber"
            receiver_logs.append(launch(index, role, nodes, count,
                                        lifetime_ms, discovery_ms,
                                        folder, files, processes))
            time.sleep(0.01)
        # Receiver-READY is a process/application-start marker, not proven
        # Discovery convergence. Actual matching and data are checked below.
        readiness_deadline = time.monotonic() + min(85, discovery_ms/1000)
        while time.monotonic() < readiness_deadline:
            if any(p.poll() is not None for p in processes.values()):
                raise RuntimeError("receiver exited before READY")
            if all("RECEIVER_READY" in count_markers(log) for log in receiver_logs):
                break
            time.sleep(.12)
        else:
            raise RuntimeError("not all receivers initialized")
        metrics["receiver_ready_count"] = len(receiver_logs)
        network_ready = net.interface_stats(nodes)
        publisher_start_ns = time.monotonic_ns()
        sender_log = launch(0, "publisher", nodes, count, lifetime_ms,
                            discovery_ms, folder, files, processes)
        ready_detected = False
        first_detected = False
        overall_deadline = time.monotonic() + min(
            150, (discovery_ms + count*150 + 12000)/1000)
        while time.monotonic() < overall_deadline:
            sender_text = count_markers(sender_log)
            if not ready_detected and "PUBLIC_DISCOVERY_READY" in sender_text:
                network_ready = net.interface_stats(nodes)
                ready_detected = True
            if not first_detected and all(
                "BENCH_FIRST_DATA" in count_markers(log)
                for log in receiver_logs[:-1]
            ):
                network_first = net.interface_stats(nodes)
                first_detected = True
            if all(p.poll() is not None for p in processes.values()):
                break
            time.sleep(.12)
        else:
            raise RuntimeError("timed out waiting for all node processes")
        network_after = net.interface_stats(nodes)
        for index in range(nodes):
            role = ("publisher" if index == 0 else
                    "negative" if index == nodes-1 else "subscriber")
            log = folder / f"node-{index:03d}-{role}.log"
            value = parse_node(log)
            reports.append({**value, "role": role, "index": index,
                            "exit": processes[index].returncode})
        publisher = reports[0]
        subscribers = reports[1:-1]
        negative = reports[-1]
        first_times = [x.get("first_received_ns", 0) for x in subscribers]
        all_first_ns = (max(first_times) if
                        len(first_times) == nodes-2 and
                        all(t > publisher_start_ns for t in first_times)
                        else None)
        total_expected = (nodes-2)*count
        total_received = sum(x.get("received", 0) for x in subscribers)
        total_duplicates = sum(x.get("duplicates", 0) for x in reports)
        total_invalid = sum(x.get("invalid", 0) for x in reports)
        metrics.update({
            "publisher_ready_ms": publisher.get("ready_ms"),
            "matched": publisher.get("matched"),
            "expected_matches": nodes-2,
            "first_data_all_ms": (
                round((all_first_ns-publisher_start_ns)/1e6, 3)
                if all_first_ns else None),
            "received": total_received, "expected_deliveries": total_expected,
            "duplicates": total_duplicates, "invalid": total_invalid,
            "negative_received": negative.get("received"),
            "processes": len(processes),
            "exits_ok": all(x.get("exit") == 0 for x in reports),
            "all_positive_complete": all(
                x.get("received") == count for x in subscribers),
            "control_and_startup_rx_bytes": (
                net.delta(network_ready, network_before)["rx_bytes"]
                if network_before and network_ready else None),
            "whole_trial_rx_bytes": (
                net.delta(network_after, network_before)["rx_bytes"]
                if network_before and network_after else None),
            "first_data_rx_bytes": (
                net.delta(network_first, network_before)["rx_bytes"]
                if network_before and network_first else None),
        })
        metrics["accepted"] = bool(
            metrics["processes"] == nodes and metrics["exits_ok"]
            and metrics["matched"] == nodes-2
            and metrics["publisher_ready_ms"] is not None
            and metrics["first_data_all_ms"] is not None
            and metrics["all_positive_complete"]
            and total_received == total_expected
            and total_duplicates == 0 and total_invalid == 0
            and metrics["negative_received"] == 0)
    except Exception as exc:
        metrics["error"] = str(exc)
    finally:
        # Always kill the *real namespace workers*, not only sudo wrappers.
        # This matters when a child detaches from its wrapper process group.
        kill_workers(nodes)
        for p in processes.values():
            if p.poll() is None:
                p.terminate()
        for p in processes.values():
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for fp in files:
            fp.close()
        try:
            net.cleanup(nodes)
        except Exception as exc:
            metrics["cleanup_error"] = str(exc)
            metrics["accepted"] = False
        metrics["elapsed_s"] = round(time.monotonic()-start, 3)
        (folder/"trial.json").write_text(
            json.dumps({"summary": metrics, "peers": reports,
                        "before": network_before, "ready": network_ready,
                        "first": network_first, "after": network_after},
                       indent=2)+"\n")
        print("DISCOVERY_SCALE_TRIAL="+json.dumps(metrics, sort_keys=True),
              flush=True)
    return metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nodes", type=int, choices=(3,10,30,50,100), required=True)
    parser.add_argument("--profile", choices=tuple(PROFILES), required=True)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--count", type=int, default=8)
    parser.add_argument("--lifetime-ms", type=int, default=105000)
    parser.add_argument("--discovery-timeout-ms", type=int, default=90000)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not (3 <= args.nodes <= LIMIT and 1 <= args.repeats <= 3 and
            1 <= args.count <= 100 and args.lifetime_ms >= 10000 and
            args.discovery_timeout_ms >= 1000):
        parser.error("invalid experiment bounds")
    args.output.mkdir(parents=True, exist_ok=True)
    trials = []
    for rep in range(1,args.repeats+1):
        trials.append(single_trial(args.nodes, args.profile, rep, args.count,
                                   args.output, args.lifetime_ms,
                                   args.discovery_timeout_ms))
    failures = [r for r in trials if not r.get("accepted")]
    summary = {
        "nodes": args.nodes, "profile": args.profile,
        "attempted": len(trials), "accepted": len(trials)-len(failures),
        "total_received": sum(x.get("received",0) for x in trials),
        "total_expected": (args.nodes-2)*args.count*len(trials),
        "trials": trials, "passed": len(failures)==0,
        "limitations": [
            "Single GitHub Actions VM with N Linux netns, not N independent machines.",
            "One publisher; N-2 positive subscribers; one negative subscriber, NOT an N-by-N mesh.",
            "One shared trusted mTLS test CA/certificate across distinct namespace processes.",
            "Readiness is all matching publishers plus every positive node receiving data; not mesh readiness.",
            "8 spaced 256-byte frames per positive subscriber; not long-running high-throughput soak.",
            "No application delivery ACK; the receive gate inspects unique payload sequences.",
            "Bridge veth RX counts all control and data bytes, not isolated discovery-only bytes."
        ],
    }
    (args.output/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
    with (args.output/"summary.csv").open("w", newline="") as f:
        keys=["nodes","profile","repetition","accepted","matched",
              "publisher_ready_ms","first_data_all_ms","received",
              "expected_deliveries","duplicates","invalid","negative_received",
              "elapsed_s","error"]
        writer=csv.DictWriter(f,fieldnames=keys,extrasaction="ignore")
        writer.writeheader()
        writer.writerows(trials)
    print("DISCOVERY_SCALE_SUMMARY="+json.dumps({
        k:v for k,v in summary.items() if k != "trials"},sort_keys=True),
        flush=True)
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
