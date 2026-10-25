# Cito SHM v1 — Publisher-Owned Ring Prototype

## Goal

Same-host delivery should remove the network hop without introducing a central broker, global shared allocator, shared reference count, or a slow-reader dependency.

The application API does not change:

```cpp
ctx.publish(message);
ctx.on<Message>(handler);
```

Transport choice remains internal.

## v1 prototype

The Linux prototype uses one POSIX shared-memory object owned by a publisher data path.

```text
publisher process
      |
      | one writer
      v
fixed bounded ring
  [slot][slot][slot]...
      ^      ^      ^
      |      |      |
 reader A reader B reader C
 read-only mappings
```

Properties:

- one writer
- many readers
- fixed slot count and fixed maximum payload size
- writer owns creation and unlink
- readers map the region with `PROT_READ`
- readers keep their cursor locally; no reader cursor/refcount is written into shared memory
- writer never waits for a reader
- a slow reader detects overwritten sequences, counts them as dropped, and resumes at the oldest still-available slot
- every slot has a sequence/stamp so a reader rejects a slot while it is being replaced
- the region carries a generation value for future publisher-restart identity

This makes reader death independent from publisher progress and from other readers.

## Slot publication

For sequence `N`:

```text
writer:
  mark slot as writing (odd stamp)
  write length + payload
  publish complete stamp for N
  publish ring write_sequence = N

reader:
  read write_sequence
  select expected slot
  read complete stamp
  copy payload
  read stamp again
  accept only if both stamps match
```

The prototype copies payload from the ring into the application's receive buffer. Zero-copy application views are deliberately deferred because they would require a lifetime/overwrite contract that must not reintroduce writer blocking or shared reference counting.

## Slow reader behavior

The ring is KEEP_LAST-like and bounded.

If the writer is at sequence 100 with 16 slots and a reader still wants sequence 70:

```text
oldest available = 85
dropped += 15
reader resumes at 85
```

The writer does not wait.

## Crash/lifecycle direction

The current Linux writer creates the POSIX SHM name with `O_EXCL` and unlinks it on normal teardown.

Production integration should derive the segment identity from a publisher/coordinator generation so a restarted publisher creates a new generation rather than silently reusing stale shared state. Crash cleanup of an old named object is therefore a control-plane lifecycle problem, not a reason to put reader ownership/refcounts into the data ring.

## Testing

Unit/latency regression verifies:

- bounded slot size
- independent reader cursors
- a slow reader does not block writes
- overwritten sequence count is reported
- reader resumes at the oldest available sequence

The Linux same-host smoke launches three independent OS processes:

```text
writer process
   |
   +--> reader process A
   |
   +--> reader process B
```

Both readers must receive 100 samples with zero drops while using their own read-only SHM mappings.

A second multi-process smoke uses the IDL-generated `Position` type. The writer uses the generated direct `StaticCodec<Position>`, places the resulting canonical Cito wire bytes in SHM, and two reader processes decode those bytes back into generated `Position` values. This proves that SHM is only a transport substitution; type identity and wire compatibility do not change.

This test intentionally does not use network namespaces because SHM is a same-host transport.

## Internal transport selection

The first internal locality selector is deliberately trivial and not public API:

```text
same process -> direct callback
same host    -> shared memory
remote       -> UDP
```

The application continues to call `publish()`. QUIC and future transport policy can extend the internal selector without creating a second application programming model.

## Deferred

- Context/runtime automatic wiring of discovered same-host endpoints to SHM rings
- generated/static wire payload integration
- publisher generation/name registry and crash cleanup
- notification primitive (eventfd/futex) instead of polling
- Windows shared-memory backend
- optional zero-copy application view with a safe bounded lifetime contract
- absolute latency/throughput benchmark against loopback UDP and other middleware
