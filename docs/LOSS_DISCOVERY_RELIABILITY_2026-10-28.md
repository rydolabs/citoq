# CitoQ lossy Discovery and authenticated QUIC reliability — 2026-10-28

## Verdict and scope

**Verified functional improvement:** Discovery snapshot-page and route-detail lookup now retry lost UDP control requests/replies at a bounded ~300ms interval while the peer's Coordinator summary and interest remain valid. The wire format, P2P routing principle, mTLS requirement, single-owner C++ runtime and Context API are unchanged.

**Not fully solved:** A separate rare asymmetric QUIC handshake/teardown case remains. In some 1%/2% loss trials, the sender observed MsQuic CONNECTED and 200 StreamSend accept/buffer completion callbacks, but the receiver accepted the connection without reaching server CONNECTED, then transport shutdown occurred; 0/200 arrived. The Discovery retry change does not explain or guarantee removal of that independent failure.

## Reproduced pre-change failures

[Baseline diagnostic run #40649158690](https://github.com/rydolabs/citoq/actions/runs/40649158690), 256B/100Hz, 200 messages/trial, independent process pairs in Linux network namespaces, 10ms±2ms netem each direction:
- **1% loss: 26/30 success**. Three 0/200 runs had no sender QUIC attempt because no matched interest/route. One 0/200 trial had sender CONNECTED and 200 accepted StreamSends, receiver accepted QUIC but server handshake never completed (transport shutdown).
- **2% loss: 28/30 success**. Both failed before any sender QUIC connect attempt, with no eligible Pub/Sub subscriber at first sample. Every rejected/partial sample is retained as a failed trial rather than hidden.
- These diagnostic builds used `-DCITOQ_LOSS_TRACE=1` to capture counts at teardown. MsQuic `SEND_COMPLETE` returns application buffer ownership and **is not an application-level delivery acknowledgement**.

## Discovery correction

Source: `include/citoq/detail/discovery_control_core.hpp` and `tests/discovery_control_core_test.cpp`, [clean fix PR #31](https://github.com/rydolabs/citoq/pull/31) stacked on previously reviewed benchmark pacing PR #27.

- Previously, `on_summary()` returned without action whenever it saw the same pending snapshot stamp. When its corresponding UDP snapshot reply was lost, no new request was issued until the stamp changed.
- Route requests were also marked “requested” when emitted, not when a validated `CTR4/CTR5` route batch was received; a lost response suppressed all future requests.
- The fix distinguishes **requested vs confirmed**, retries only the last missing snapshot page and still-unconfirmed route keys after ~300ms on repeated Coordinator summaries, clears retry intent on authenticated/validated control response, route release, coordinator incarnation change, and lease expiry.
- The test deliberately drops the first snapshot and route request/reply, observes one bounded retry after 300ms, then checks no extra route chatter after confirmation. Existing stale-route rejection and lifecycle assertions remain.
- No per-message transport modifications, polling busy-spin, backpressure change, or certificate validation weakening.

## Strict results: all network profiles verified from actual tc output

The initial clean-run 2% result appeared as 30/30 with ~0.30ms p95. **That result was invalid**, because the benchmark runner had not applied the 2% qdisc. It is expressly excluded. The runner now recognizes both loss profiles and *fails closed* if `tc -s qdisc show dev eth0` does not indicate the selected loss percentage in **both** namespaces. `benchmark/test_network_profiles.py` validates both true 1% and 2%, rejects mismatched/unknown profiles and unexpected baseline netem.

| True loss, baseline vs updated code | Baseline diagnostic | Clean source, earlier valid attempt | Clean source, latest fully valid attempt |
| --- | ---: | ---: | ---: |
| 1% | 26/30 | 30/30 | **30/30** |
| 2% | 28/30 | **28/30** (117/200 and 0/200 failed) | **30/30** |

Earlier clean run: [#40651564818](https://github.com/rydolabs/citoq/actions/runs/40651564818); later fully valid run: [#40651652055](https://github.com/rydolabs/citoq/actions/runs/40651652055). Both passed explicit kernel loss verification. Final run delivered **12,000/12,000** unique valid messages across 60 trials with zero missing, duplicates, invalid data or nonzero exits.

Final median-of-trial p95 under actual loss: **1% 11.925544ms**, **2% 12.093231ms**; p99 **1% 12.127187ms**, **2% 35.843013ms**. The 2% tail still has substantially delayed cases. One 30/30 pass **does not prove elimination of intermittent failures**; the preceding valid 28/30 on the exact source shows a remaining stability concern.

A separate instrumentation-enabled candidate [#40650107658](https://github.com/rydolabs/citoq/actions/runs/40650107658) improved Discovery matching to 29/30 under each loss profile; both isolated failures were server-side QUIC handshake/transport shutdown, not missing Discovery.

## Forced sender process crash + incarnation restart

The clean test-only [PR #33](https://github.com/rydolabs/citoq/pull/33) and [CI #40651620976](https://github.com/rydolabs/citoq/actions/runs/40651620976) pass **3/3 independent trials at actual 1% and 3/3 at actual 2% loss**. Existing receiver remains alive; sender incarnation 2001 publishes 30 frames then receives SIGKILL; new sender incarnation 2002 reconnects with trusted mTLS certs, and delivers all **200 new** IDs 31..230 per test: **1,200/1,200 post-restart**, zero duplicates/corruption. This does *not* imply the 30 pre-crash frames were durable or that receiver/Coordinator restarts have been tested.

## Other CI and release boundaries

- [Source CTest #40650906174](https://github.com/rydolabs/citoq/actions/runs/40650906174): 67/67 passed on clean implementation.
- [Native authenticated G1 #40650906239](https://github.com/rydolabs/citoq/actions/runs/40650906239) and [Python mTLS G4 #40650906163](https://github.com/rydolabs/citoq/actions/runs/40650906163): passed.
- Shared GitHub Actions Ubuntu runner/netns/veth, not physical hosts; only 200-message trials, offered 100Hz, not sustained throughput or formal fault tolerance proof.
- StreamSend accepted / SEND_COMPLETE do not mean peer application ACK. Ending a publishing process before peer acknowledgement can lose data under network impairment. Reliable crash survival would require explicit delivery semantics; no attempt to add new on-wire ACK or change the user's API was made in this fix.
- A separate bounded sender FIN-ACK drain experiment [PR #34](https://github.com/rydolabs/citoq/pull/34) was created for review, **not enabled**. No inference about it can be drawn before its CI completes.
- Do not deploy or merge these stacked PRs as a full “guaranteed reliable in any loss/restart condition” claim. Prioritize QUIC shutdown status analysis, receiver and Coordinator restart tests, long-duration validation and rollout review before declaring all acceptance gates complete.

## Methods and artifact navigation

- [30× baseline tracing](https://github.com/rydolabs/citoq/actions/runs/40649158690), [30× after Discovery fix with tracing](https://github.com/rydolabs/citoq/actions/runs/40650107658)
- [Clean default-build real-loss 30× matrix](https://github.com/rydolabs/citoq/actions/runs/40651652055), [clean restart recovery](https://github.com/rydolabs/citoq/actions/runs/40651620976)
- The `tc` qdisc descriptions, raw sender/receiver logs, successful and failed per-trial JSON summaries, p50/p95/p99, CPU and RSS are preserved as GitHub Actions artifacts.
