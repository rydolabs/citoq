"""CitoQ import facade over the existing native Cito Python binding.

The native shared library must be installed or exposed using CITO_LIBRARY.
"""
import cito as _legacy
__all__ = list(_legacy.__all__)

def __getattr__(name):
    if name in __all__:
        return getattr(_legacy, name)
    raise AttributeError("module 'citoq' has no attribute " + repr(name))
