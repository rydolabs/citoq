from . import _wire

__all__ = [
    "Context",
    "Subscription",
    "HandlerRegistration",
    "RequestError",
    "Cancelled",
    "Unavailable",
    "DeadlineExceeded",
    "abi_version",
]


def __getattr__(name: str):
    if name in {
        "Context",
        "Subscription",
        "HandlerRegistration",
        "RequestError",
        "Cancelled",
        "Unavailable",
        "DeadlineExceeded",
        "abi_version",
    }:
        from ._native import (
            Context,
            Subscription,
            HandlerRegistration,
            RequestError,
            Cancelled,
            Unavailable,
            DeadlineExceeded,
            abi_version,
        )

        globals().update({
            "Context": Context,
            "Subscription": Subscription,
            "HandlerRegistration": HandlerRegistration,
            "RequestError": RequestError,
            "Cancelled": Cancelled,
            "Unavailable": Unavailable,
            "DeadlineExceeded": DeadlineExceeded,
            "abi_version": abi_version,
        })
        return globals()[name]

    raise AttributeError(name)
