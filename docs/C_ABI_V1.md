# Cito C ABI v1

Cito exposes a stable C boundary for language bindings and non-C++ applications.

The v1 boundary is Context + type_id + resource + bytes. It does not expose STL types, C++ exceptions, RTTI, templates, or C++ object layouts.

Opaque Context and Subscription handles use explicit destruction. Strings and payloads are pointer + length. Callback pointers are borrowed and valid only during the callback.

ABI version starts at 1. Existing signatures are not changed after stabilization; new capabilities use new functions or versioned option structs.
