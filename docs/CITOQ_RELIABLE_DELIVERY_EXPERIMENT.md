# CitoQ QUIC application-ACK Reliable mode — experimental decision gate

## Motivation

Baseline discovery can find all 198/198 matching subscribers and reach
198/198 QUIC sessions without delivering all 1,584 application messages in a
200-netns lossy run. MsQuic SEND_COMPLETE is not proof the subscriber's CitoQ
callback accepted a message. Existing `include/citoq/detail/reliability.hpp`
and `reliable_packet.hpp` implement history/NACK protocol primitives but are
not wired into the public native QUIC data path.

## Branch implementation

Build with `-DCMAKE_CXX_FLAGS=-DCITOQ_RELIABLE_QUIC=1` to activate the
experimental reliable native CitoQ QUIC transport. A normal build
(`CITOQ_RELIABLE_QUIC=0`) retains CTD1 Best-effort behavior. The mode is
compile-time and **not negotiated**; all senders and receivers of a test must
use matching builds. This is a proof of concept, not a stable/public QoS API.

- Sender wraps CTD1 in CTY1 with a process-scoped random sender epoch
  and sequence number; stores up to **4,096** unacknowledged frames or
  **16 MiB** of framed data per publisher process.
- The authenticated receiving QUIC endpoint delivers the inner CTD1 to
  the CitoQ application callback, then sends a compact CTK1 ACK on the
  same bidirectional QUIC stream. It does not ACK a refused/unmatched frame.
- Receiver deduplicates `(sender_epoch,sequence)` within a bounded window
  (4,096 recent sequences per epoch, up to 128 epochs).
- Sender retries after 650 ms with exponential backoff capped at 3.2 s;
  attempts are bounded to at most six additional sends and 14 seconds.
  An ACK is credited only for the same epoch, sequence and destination.
- Debug counters: outstanding, acked, retried, expired and rejected frames.
- Network benchmark requires both **all subscriber callbacks** and **all
  sender-received application ACKs** in Reliable mode.

### Explicit limitations

This only acknowledges that the synchronous CitoQ callback was invoked.
It is **not** disk durability and does not imply an application committed
a database transaction. Receiver deduplication is in-memory and bounded,
not persistent exactly-once delivery across restarts or more than 4,096
reordered samples. ACKs are authenticated only by the existing mTLS
connection; application authorization is separate. A full production
implementation needs QoS negotiation, per-destination quotas, application
failure signaling, replay protection across restarts, bounded retry fair
scheduling, and proper public API semantics.

## Fair comparison and decision gate

Workflow: `.github/workflows/benchmark-citoq-reliable-200netns.yml`.

Hold source commit, compiler, network, 1 publisher, N-2 subscribers, one
negative-interest node, mTLS, 8 x 256B messages/subscriber,
10 +/- 2ms per-namespace delay, 0%, 1%, 2% packet loss, lease 1500ms
and announcement 100ms constant. Change **only**
`CITOQ_RELIABLE_QUIC=0/1`. Both compare modes use passive Discovery and
the full QUIC session-readiness barrier; reconnect extension is disabled
for both. All tests are one Ubuntu Actions VM with Linux namespaces, not
independent hosts. Matrix: 2 sizes x 3 losses x 2 modes x 2 repeats =
24 screening trials.

Expected demonstration of value: 200-netns loss 1-2% Reliable succeeds
where Best-effort fails **without unbounded ACK history**. Record complete
subscriber delivery, duplicate count, sender-ACK count, readiness times,
per-trial wire RX, CPU and memory.

If screening achieves no end-to-end improvement, do not merge.
If it is promising, repeat 10+ independent runs per condition and add
explicit packet drop of ACKs, 100% network partition, process restart,
long-running soak, memory ceilings, and a multi-publisher collision test
before considering implementation beyond an experiment.
