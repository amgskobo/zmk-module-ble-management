# BLE management handler contracts

From an already resolved owner workspace:

```sh
python3 tests/split-info/run.py
```

`NANOPB_SOURCE` and `ZEPHYR_BASE` can select explicit, previously inspected
nanopb and Zephyr checkouts.
The runner copies its generator to a temporary directory, keeps the dependency
read-only, and builds the complete production handler and label store with
controlled Zephyr, ZMK and Studio-allocation APIs. It compiles the actual Zephyr
Bluetooth address implementation and does not copy the module's implementation.

Studio RPC runs only on a standalone keyboard or a split central, so those are
the roles built. The actual request and response schemas/codecs must preserve
the containing `SplitInfo` message, including an all-false info message. On a
central, `peripheral_connected` is true only for a connected central-role link:
a host link, or a half that is still connecting or already disconnecting, does
not count. `central_bonded`, a peripheral's view, stays false. An invalid
request must not reuse the previous response. Both optimized and ASan/UBSan
variants run.

Storage tests distinguish public, random, public identity and random identity
addresses with the same MAC; check typed and legacy keys in both load orders;
and verify malformed, short and failed reads cannot poison the live cache.
Typed keys win over legacy public keys, including an explicitly empty name.
Label tests check that unpairing deletes both stored keys and frees the slot, an
empty name deletes the label, saving a typed key retires the legacy key once,
and a full store reclaims the label of an unbonded host (deleting its record)
but refuses when every label still belongs to a bonded host. Clearing all host
pairings deletes their labels. The controlled settings backend checks bounded
values and the existing RAM-before-storage behavior.

Profile tests encode the actual response at five profiles and verify a six-profile
configuration returns `-E2BIG` before writing its fixed-capacity response array.
`--case split-info|storage-key|storage-read|labels|profiles|forget-split|unlock`
selects a focused group; the default `all` runs them together across the
supported roles. Warning exceptions keep the original source's out-of-bounds
negative control executable under UBSan; other warnings remain errors.
`--source` and `--labels-source` build other copies of the two sources, for
negative controls that undo one change in a scratch copy.

This pins the module RPC payload, not the enclosing Studio transport or a
physical bond/connection. Keep owner registration/build tests and physical
acceptance separate; this regression does not establish reconnection, storage or
reset behavior and does not exercise a real bond-clear operation.
