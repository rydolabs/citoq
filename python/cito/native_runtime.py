"""Linux native QUIC runtime for Cito's Python C ABI.

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
        library = os.environ.get("CITO_NATIVE_LIBRARY")
        if not library:
            raise RuntimeError("CITO_NATIVE_LIBRARY must identify the Linux native transport")
        self._lib = ctypes.CDLL(library)
        self._lib.cito_native_open.argtypes = [ctypes.POINTER(_Options)]
        self._lib.cito_native_open.restype = ctypes.c_void_p
        self._lib.cito_native_poll.argtypes = [ctypes.c_void_p]
        self._lib.cito_native_poll.restype = ctypes.c_size_t
        self._lib.cito_native_close.argtypes = [ctypes.c_void_p]
        self._options = _Options(
            coordinator, incarnation, destination,
            control_port, quic_port, announcement_port,
            _b(announcement_ipv4), _b(server_cert), _b(server_key),
            _b(client_ca), _b(trusted_server_ca),
            _b(client_cert), _b(client_key))
        self._handle = self._lib.cito_native_open(ctypes.byref(self._options))
        if not self._handle:
            raise RuntimeError("Native Cito QUIC transport could not start")

    def poll(self) -> int:
        if not self._handle:
            raise RuntimeError("Native runtime already closed")
        return int(self._lib.cito_native_poll(self._handle))

    def close(self) -> None:
        if self._handle:
            self._lib.cito_native_close(self._handle)
            self._handle = None

    def __enter__(self) -> "NativeRuntime":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()
