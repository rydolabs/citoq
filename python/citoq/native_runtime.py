"""Linux native QUIC runtime for CitoQ's Python C ABI.

Use a single owner thread to poll. Contexts must be closed before this runtime.
The transport authenticates mTLS peers; production identity/ACL provisioning
remains a separate integration requirement.
"""
from __future__ import annotations
import ctypes
import os

class _Options(ctypes.Structure):
    _fields_ = [
        ("coordinator", ctypes.c_uint64),
        ("incarnation", ctypes.c_uint64),
        ("destination", ctypes.c_uint64),
        ("control_port", ctypes.c_uint16),
        ("quic_port", ctypes.c_uint16),
        ("announcement_port", ctypes.c_uint16),
        ("announcement_ipv4", ctypes.c_char_p),
        ("server_cert", ctypes.c_char_p),
        ("server_key", ctypes.c_char_p),
        ("client_ca", ctypes.c_char_p),
        ("trusted_server_ca", ctypes.c_char_p),
        ("client_cert", ctypes.c_char_p),
        ("client_key", ctypes.c_char_p),
    ]


def _b(value: str | None) -> bytes | None:
    return value.encode("utf-8") if value else None


class NativeRuntime:
    def __init__(
        self,
        *,
        coordinator: int,
        incarnation: int,
        destination: int = 0,
        control_port: int,
        quic_port: int = 0,
        announcement_ipv4: str | None = None,
        announcement_port: int = 0,
        server_cert: str | None = None,
        server_key: str | None = None,
        client_ca: str | None = None,
        trusted_server_ca: str | None = None,
        client_cert: str | None = None,
        client_key: str | None = None,
    ):
        library = os.environ.get("CITOQ_NATIVE_LIBRARY")
        if not library:
            raise RuntimeError("CITOQ_NATIVE_LIBRARY must identify the Linux native transport")
        self._lib = ctypes.CDLL(library)
        self._lib.citoq_native_open.argtypes = [ctypes.POINTER(_Options)]
        self._lib.citoq_native_open.restype = ctypes.c_void_p
        self._lib.citoq_native_poll.argtypes = [ctypes.c_void_p]
        self._lib.citoq_native_poll.restype = ctypes.c_size_t
        # Keep poll() usable with older native libraries that lack this new,
        # optional symbol; only poll_wait() itself requires a matching library.
        self._poll_wait_fn = getattr(self._lib, "citoq_native_poll_wait", None)
        if self._poll_wait_fn is not None:
            self._poll_wait_fn.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            self._poll_wait_fn.restype = ctypes.c_size_t
        self._lib.citoq_native_close.argtypes = [ctypes.c_void_p]
        self._options = _Options(
            coordinator, incarnation, destination,
            control_port, quic_port, announcement_port,
            _b(announcement_ipv4), _b(server_cert), _b(server_key),
            _b(client_ca), _b(trusted_server_ca),
            _b(client_cert), _b(client_key))
        self._handle = self._lib.citoq_native_open(ctypes.byref(self._options))
        if not self._handle:
            raise RuntimeError("Native CitoQ QUIC transport could not start")

    def poll(self) -> int:
        if not self._handle:
            raise RuntimeError("Native runtime already closed")
        return int(self._lib.citoq_native_poll(self._handle))

    def poll_wait(self, max_wait_ms: int = 10) -> int:
        """Wait for a socket/QUIC event and poll on this same owner thread.

        Unlike busy polling, this sleeps in the kernel until a wakeup/deadline.
        Call from one thread only; callbacks run synchronously on this thread.
        """
        if not self._handle:
            raise RuntimeError("Native runtime already closed")
        if not isinstance(max_wait_ms, int) or not 0 <= max_wait_ms <= 0xFFFFFFFF:
            raise ValueError("max_wait_ms must be an integer in [0, 2**32-1]")
        if self._poll_wait_fn is None:
            raise RuntimeError("poll_wait requires a newer native CitoQ runtime library")
        return int(self._poll_wait_fn(self._handle, max_wait_ms))

    def close(self) -> None:
        if self._handle:
            self._lib.citoq_native_close(self._handle)
            self._handle = None

    def __enter__(self) -> "NativeRuntime":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()
