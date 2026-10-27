"""Fast, isolated tests for optional NativeRuntime.poll_wait ABI behavior."""
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "citoq_native_runtime_abi_test_module",
    ROOT / "python" / "citoq" / "native_runtime.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NativePollWaitApiTest(unittest.TestCase):
    def fake(self, handle=123, fn=None):
        runtime = MODULE.NativeRuntime.__new__(MODULE.NativeRuntime)
        runtime._handle = handle
        runtime._poll_wait_fn = fn
        return runtime

    def test_poll_wait_passes_timeout_to_owner_thread_function(self):
        recorded = []
        runtime = self.fake(fn=lambda handle, ms: recorded.append((handle, ms)) or 3)
        self.assertEqual(runtime.poll_wait(7), 3)
        self.assertEqual(recorded, [(123, 7)])

    def test_missing_optional_symbol_errors_only_on_new_method(self):
        runtime = self.fake()
        with self.assertRaisesRegex(RuntimeError, "newer native CitoQ runtime"):
            runtime.poll_wait(10)

    def test_closed_runtime_rejected(self):
        runtime = self.fake(handle=None, fn=lambda *_: 1)
        with self.assertRaisesRegex(RuntimeError, "already closed"):
            runtime.poll_wait()

    def test_invalid_timeouts_rejected(self):
        runtime = self.fake(fn=lambda *_: 1)
        for ms in (-1, 1 << 32, 0.5, "10"):
            with self.subTest(ms=ms), self.assertRaises(ValueError):
                runtime.poll_wait(ms)
        self.assertEqual(runtime.poll_wait(0), 1)


if __name__ == "__main__":
    unittest.main()
