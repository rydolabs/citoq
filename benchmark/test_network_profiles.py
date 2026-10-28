"""Fail-closed proof: benchmark loss profile must match applied kernel tc qdisc."""
import importlib.util
import pathlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SPEC=importlib.util.spec_from_file_location(
    "network_profile_driver",pathlib.Path(__file__).parent/"run_comparison.py")
module=importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name]=module
SPEC.loader.exec_module(module)

class NetworkProfileTest(unittest.TestCase):
    def run_profile(self,profile,applied):
        calls=[]
        def fake_cmd(*parts,check=True):
            calls.append(parts)
            if "qdisc" in parts and "show" in parts:
                return types.SimpleNamespace(stdout=applied)
            return types.SimpleNamespace(stdout="")
        with tempfile.TemporaryDirectory() as path, \
             patch.object(module,"OUT",pathlib.Path(path)), \
             patch.object(module,"cmd",side_effect=fake_cmd):
            module.set_profile(profile)
        return calls

    def test_one_percent_is_installed_on_both_namespaces(self):
        calls=self.run_profile("netem_10ms_1pct",
            "qdisc netem 8001: root refcnt 2 delay 10ms 2ms loss 1%")
        self.assertEqual(sum("loss" in x and "1%" in x for x in calls),2)

    def test_two_percent_is_installed_on_both_namespaces(self):
        calls=self.run_profile("netem_10ms_2pct",
            "qdisc netem 8001: root refcnt 2 delay 10ms 2ms loss 2%")
        self.assertEqual(sum("loss" in x and "2%" in x for x in calls),2)

    def test_wrong_profile_can_never_pass(self):
        with self.assertRaises(RuntimeError):
            self.run_profile("netem_10ms_2pct",
                "qdisc netem 8001: root refcnt 2 delay 10ms 2ms loss 1%")
        with self.assertRaises(RuntimeError):
            self.run_profile("netem_10ms_2pct","qdisc noqueue 0: root")

    def test_baseline_rejects_netem(self):
        with self.assertRaises(RuntimeError):
            self.run_profile("baseline","qdisc netem 8001: root loss 1%")
        self.run_profile("baseline","qdisc noqueue 0: root")

    def test_unknown_profile_is_error_not_implicit_baseline(self):
        with self.assertRaises(ValueError):
            self.run_profile("netem_10ms_3pct","")

if __name__=="__main__":
    unittest.main()
