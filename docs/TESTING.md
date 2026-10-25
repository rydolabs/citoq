# Cito Testing Policy

Every Cito development phase must pass three mandatory test gates before the phase is considered complete:

1. **Unit tests** for the behavior introduced or changed in that phase.
2. **Regression tests** containing all previously accepted stable tests plus the new stable tests.
3. **Smoke tests** proving the smallest realistic user-visible flow for that phase.

Implementation without all three gates passing is not phase completion.

## Test meanings

### Unit

Focused tests for one component or rule, for example:

- TypeId / SchemaHash / FieldId behavior
- codec encode/decode rules
- schema compatibility
- InterestIndex operations
- lease/timeout state machines
- SHM ring mechanics
- reliability state machines

Run with:

```bash
ctest --test-dir build -L unit --output-on-failure
```

### Regression

The cumulative suite. Every accepted test remains in the regression set unless it is deliberately replaced by a stronger equivalent test.

A new feature is not allowed to break an older API, wire rule, discovery rule, or transport behavior silently.

Run with:

```bash
ctest --test-dir build -L regression --output-on-failure
```

### Smoke

A minimal end-to-end flow proving that the phase works through the intended public/runtime path rather than only through isolated internals.

Examples:

- P0/API: create Context -> subscribe -> publish -> callback
- Type/Wire: generated/static value -> encode -> decode -> expected value
- UDP: namespace A publishes -> namespace B receives
- discovery: interested node receives while unrelated node does not
- SHM: process A publishes -> process B receives through SHM
- reliable: injected packet loss -> bounded recovery succeeds
- language binding: Python publishes -> C++ receives

Run local smoke tests with:

```bash
ctest --test-dir build -L smoke --output-on-failure
```

## Local-first rule

Tests run in the current development session whenever the behavior can be validated faithfully there.

Typical local work:

- unit tests
- schema/wire regression
- same-process API smoke
- discovery simulations
- malformed-input tests
- sanitizer runs
- local benchmarks

GitHub Actions is used only when the test requires an environment that is not faithfully available in the current session.

## GitHub Actions network rule

When independent network nodes are required, use Linux network namespaces on one Ubuntu runner:

```text
runner
  |
br-cito
 / | \
A  B  C
```

Each namespace gets its own IP/interface/socket namespace. Start with 3-node smoke tests, then 10/30-node regression tests as needed.

Use `tc/netem` only when fault injection is required.

Network namespace results are correctness/regression evidence, not physical-network performance claims.

## SHM rule

Same-host SHM tests use multiple real OS processes on the same runner/host, not network namespaces.

## Phase completion checklist

For every P-stage:

- implementation compiles with supported local compilers where available
- new unit tests pass
- the complete regression suite passes
- the phase smoke test passes
- sanitizer checks run where meaningful
- GitHub Actions is used only if independent nodes/OS-specific behavior are required
- failures are reproduced locally when possible before changing code
- documentation is updated with the tested contract

## Current labels

- `unit`: focused component/rule tests
- `smoke`: minimal realistic flows and scale sanity checks
- `latency`: deterministic structural guards against known latency regressions\n- `regression`: all stable tests that must continue passing

Tests may carry more than one label. In particular, accepted unit and smoke tests normally also belong to `regression`.
