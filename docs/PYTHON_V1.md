# Cito Python v1

The Python binding sits on the C ABI rather than the C++ ABI and initially exposes the common type_id + resource + bytes boundary.

Example:

    ctx = cito.Context('acme')
    sub = ctx.on(100, handler, 'drone_17')
    ctx.publish(100, b'hello', 'drone_17')

IDL-generated typed Python helpers are a later layer over the same runtime and wire contract.


## Typed generated API

Generated IDL classes can now be used directly with the Python Context:

```python
import cito
from position import Position

ctx = cito.Context("acme")
sub = ctx.on(
    Position,
    handler,
    "drone_17")

ctx.publish(
    Position(
        x=1.0,
        y=2.0,
        frame="map"),
    "drone_17")
```

The binding derives the TypeId from the generated class, encodes with the canonical Cito wire codec, sends through the stable C ABI, and decodes the callback payload back into the generated class.

The raw compatibility surface remains available:

```python
ctx.on_raw(type_id, bytes_handler, resource)
ctx.publish_raw(type_id, payload, resource)
```

The distributed-network runtime is not yet attached below the C ABI; this milestone removes raw bytes/TypeId from normal Python application code while preserving the same ABI boundary.
