"""Deterministic pacing regression: discovery must not flush overdue ticks."""
import importlib.util
import pathlib
import sys
import types
import unittest
from unittest.mock import patch

SPEC=importlib.util.spec_from_file_location(
    "pacing_probe_driver",pathlib.Path(__file__).parent/"bench_driver.py")
mod=importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name]=mod
SPEC.loader.exec_module(mod)


class VirtualClock:
    def __init__(self, jitter_ns=0):
        self.ns=1_000_000_000
        self.jitter_ns=jitter_ns

    def now_ns(self):
        return self.ns

    def monotonic(self):
        return self.ns/1e9

    def sleep(self, seconds):
        assert seconds >= 0
        self.ns+=int(seconds*1e9)+self.jitter_ns


class PacingRebaseTest(unittest.TestCase):
    def measure(self, mode, jitter_ns=0, stall_on=0):
        clock=VirtualClock(jitter_ns)
        sends=[]
        attempted=[0]
        args=types.SimpleNamespace(
            count=12,rate=1000,payload=256,timeout=5,
            engine="citoq",citoq_poll_mode="immediate",
            pacing_mode=mode,pacing_trace=False)
        def send(payload):
            attempted[0]+=1
            if attempted[0] == 1:
                # First publish refused until topology discovery, 30ms later.
                clock.ns+=30_000_000
                return 0
            seq,stamp=mod.STRUCT.unpack_from(payload)
            sends.append((seq,stamp))
            if seq == stall_on:
                clock.ns += 4_000_000
            return 1
        with patch.object(mod,"now_ns",side_effect=clock.now_ns), \
             patch.object(mod.time,"monotonic",side_effect=clock.monotonic), \
             patch.object(mod.time,"sleep",side_effect=clock.sleep):
            mod.paced_send(args,send,service=lambda:None,wait_for_match=True)
        self.assertEqual(len(sends),12)
        self.assertEqual([seq for seq,_ in sends],list(range(1,13)))
        return [b[1]-a[1] for a,b in zip(sends,sends[1:])]

    def test_legacy_replays_deadlines_after_topology_discovery(self):
        self.assertGreaterEqual(sum(gap==0 for gap in self.measure("legacy")),10)

    def test_rebase_preserves_one_millisecond_spacing(self):
        self.assertEqual(self.measure("rebase"),[1_000_000]*11)

    def test_rebase_keeps_target_rate_despite_70us_sleep_jitter(self):
        gaps = self.measure("rebase",jitter_ns=70_000)
        self.assertGreater(11*1e9/sum(gaps),975)
        self.assertTrue(all(gap>=850_000 for gap in gaps))

    def test_rebase_skips_delayed_tick_without_catchup_burst(self):
        gaps = self.measure("rebase",stall_on=5)
        self.assertTrue(all(gap>=900_000 for gap in gaps))
        self.assertGreater(max(gaps),4_000_000)


if __name__=="__main__":
    unittest.main()
