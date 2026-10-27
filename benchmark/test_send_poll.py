"""Deterministic regression: CitoQ send queue must be serviced after publish."""
import importlib.util
import pathlib
import types
import unittest

SPEC=importlib.util.spec_from_file_location("bench_driver",pathlib.Path(__file__).parent/"bench_driver.py")
mod=importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name]=mod
SPEC.loader.exec_module(mod)

class PollOrderingTest(unittest.TestCase):
    def exercise(self, mode):
        events=[]
        args=types.SimpleNamespace(count=2,rate=1000,payload=256,timeout=1,
                                   engine="citoq",citoq_poll_mode=mode)
        def send(payload):
            self.assertEqual(len(payload),256)
            events.append("publish")
            return 1
        mod.paced_send(args,send,service=lambda:events.append("poll"),wait_for_match=True)
        return events

    def test_immediate_mode_services_outbound_queue(self):
        self.assertEqual(self.exercise("immediate"),
                         ["poll","publish","poll","poll","publish","poll"])

    def test_legacy_mode_proves_old_100hz_delay(self):
        self.assertEqual(self.exercise("legacy"),
                         ["poll","publish","poll","publish"])

if __name__=="__main__":
    unittest.main()
