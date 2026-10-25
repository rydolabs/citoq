# Cito Reliability v1 — Selective Recovery Prototype

## Goal

Reliability is optional. BestEffort stays the default and must not pay sender-history, ACK, retry, or recovery-state cost.

The first reliable prototype targets continuous streams over a lossy datagram transport.

## Normal path

Healthy traffic does not generate one ACK per sample.

```text
sender DATA(1) ----> receiver
sender DATA(2) ----> receiver
sender DATA(3) ----> receiver

normal ACK traffic = 0
```

The receiver tracks a bounded sequence window. A missing sequence creates a selective NACK only when loss is observed.

## Control model

The state-machine prototype uses four concepts:

```text
DATA(seq, payload)
HEARTBEAT(first_available, last_published)
NACK(base, 64-bit missing bitmap)
GAP(first, last)
```

- middle loss is detected when a later DATA sequence arrives
- tail loss is detected from HEARTBEAT because no later DATA exists to expose the gap
- NACK covers at most 64 sequences from `base`
- requests still in sender history become retransmit actions
- requests older than retained KEEP_LAST history become GAP
- future/malformed NACK requests do not grow state

The first wire framing is now defined for the prototype. Multicast NACK suppression and fragmentation remain deferred.

## Reliable stream identity

Reliability state is scoped by:

```text
DemandKey (Scope + Resource + Type)
        +
opaque StreamId
```

`StreamId` identifies one publisher stream/generation. It must change when that publisher stream is recreated so two publishers of the same DemandKey never share a sequence space.

The prototype frames are:

```text
CRD1 DATA       DemandKey + StreamId + seq + payload_length + payload
CRH1 HEARTBEAT  DemandKey + StreamId + first_available + last_published
CRN1 NACK       DemandKey + StreamId + base + 64-bit bitmap
CRG1 GAP        DemandKey + StreamId + first + last
```

DATA decode exposes a non-owning payload span, avoiding another transport-frame payload copy before the type decoder.

## Bounded sender history

`SenderHistory(sample_capacity, max_payload_size)` uses a fixed sample-slot ring.

At construction each slot reserves its maximum payload capacity. Runtime publication therefore reuses bounded slot storage instead of letting history grow with process lifetime or receiver lag.

```text
capacity = 4

published: 1 2 3 4 5 6

retained:
        3 4 5 6

NACK 1..6
 -> GAP 1..2
 -> retransmit 3,4,5,6
```

The writer never extends history to satisfy an old reader.

## Bounded receiver window

The receiver uses a fixed sequence-stamp ring. It does not allocate one object per missing sequence.

A packet farther ahead than the configured window is not stored. Recovery proceeds from the current window through later NACK/heartbeat cycles.

## Current tests

Unit tests cover:

- fixed sender history eviction
- compact GAP generation for requests outside history
- middle-gap selective NACK
- heartbeat tail-loss detection
- duplicate/in-window advancement
- far-ahead bounded receive behavior

Smoke tests cover:

- one dropped middle sample -> one NACK -> one retransmit -> convergence
- dropped final sample -> HEARTBEAT -> one NACK -> convergence
- late receiver beyond KEEP_LAST -> GAP + retained retransmits -> convergence

All tests are local because the algorithm does not require independent network nodes.

## Next reliability steps

1. add NACK suppression/random delay for multicast
2. add retransmit pacing/token-bucket bounds
3. run Linux namespace tests with `tc/netem` loss/reorder/delay
4. add payload fragmentation only after MTU/GSO strategy is measured
5. keep QUIC streams separate: do not add this retransmission layer on top of QUIC reliable streams
6. add a separate bounded ACK/retry policy only if important discrete-message use cases require it
