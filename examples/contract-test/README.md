# Control-API conformance suite

`contract_test.py` drives a live MoxRelay instance over its control endpoint
(`ws://127.0.0.1:{port}/control`) and checks the observed wire behavior against the
contract in `docs/control-api.asyncapi.yaml`: the request/reply framing, all methods,
the protocol and application error codes, per-connection event gating, and the
reconnect (no-session, re-subscribe, re-snapshot) rules.

It doubles as a **reference client**: the embedded `WsClient` class is a minimal,
dependency-free implementation of the wire (Python standard library only).

## Run

1. Start a MoxRelay instance and note its control port -- the discovery file
   `%APPDATA%/MoxRelay/helper-config.json` carries the bare object's `port`.
2. ```
   python contract_test.py <port>
   ```

Exit code `0` means every check passed.

## Scope notes

- The suite creates its own color source for the mutation checks and removes it
  again; sources it did not create are left untouched.
- `GetFleet` / `fleetChanged` are gone (single-instance contract): the suite
  asserts `GetFleet` replies `-32601` and that `GetStatus` carries
  `publishedSenderNames[]`.
- `instanceShuttingDown` requires shutting the instance down mid-suite and is
  intentionally not covered here.
