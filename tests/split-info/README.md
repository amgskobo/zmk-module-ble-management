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
variants run. Every compiled variant executes three times in separate processes.

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

Reclaim-failure controls cover both a refused typed-key deletion and a partially
completed typed/legacy deletion. Neither may reuse the slot or save a new label;
the old cache and independent names stay intact, and a successful retry reclaims
the slot. Open-profile controls use the selected core's `BT_ADDR_LE_ANY` sentinel,
also reject the invalid `BT_ADDR_LE_NONE` peer, and retain a bonded-peer control.

Profile tests encode the actual response at five profiles and verify a six-profile
configuration returns `-E2BIG` before writing its fixed-capacity response array.
`--case split-info|storage-key|storage-read|labels|profiles|forget-split|unlock|reclaim-failure|open-profile|peer-identity`
selects a focused group; the default `all` runs them together across the
supported roles. Warning exceptions keep the original source's out-of-bounds
negative control executable under UBSan; other warnings remain errors.
`--source` and `--labels-source` build other copies of the two sources, for
negative controls that undo one change in a scratch copy.
`--output` retains generated inputs and binaries in a new directory and refuses
an existing directory. Compiler commands and binary SHA-256 values are recorded
before each variant's three executions.

The peer-identity group sends the real codec's complete address/type identity
and rejects legacy index-only requests, replaced MACs and replaced address types
without changing names, selecting profiles or clearing bonds. A matching current
peer remains a positive rename/unpair control. Locked lists omit both forms of
the peer address. A control that removes the identity comparison must fail the
same wrong-peer mutation assertion after a successful compile.

Five additional groups run in the complete `all` entry and are individually
selectable with `--case rename-reclaim|label-save-peer|label-delete-peer|unpair-selection|unpair-replacement`.
The settings-deletion hook replaces a synthetic borrowed profile address while
the full label store reclaims a stale record, or between typed and legacy-key
deletions. It models the actual selected Zephyr `settings_delete()` boundary,
which can wait for `settings_lock`; it does not invent a wait inside address
comparison. Both the RPC rename and direct label APIs must retain the original
typed peer and preserve independent names.

The selection hook changes the active index or replaces the requested peer
before `zmk_ble_prof_select()` returns. The selected DYA implementation can
wait in advertising's synchronous HCI command at that boundary. Unpair must
then return an error without clearing a bond, deleting a label or restoring
another selection. The ordinary matching inactive-peer case still requires a
successful clear, label deletion, two selection calls and restoration to the
original active index. All old assertions remain.

For isolated source controls, invoke the unchanged helper against a distinct
disposable container owner copy:

```sh
python3 tests/review_negative_control.py FAULT "$ISOLATED_OWNER"
python3 tests/split-info/run.py \
  --source "$ISOLATED_OWNER/src/studio/ble_management_handler.c" \
  --labels-source "$ISOLATED_OWNER/src/profile_labels.c" \
  --case CASE --output "$OUT"
```

| Fault | Required case and runtime assertion |
| --- | --- |
| `rename-reclaim` | `rename-reclaim`: `Rename changed its peer during settings reclaim` |
| `label-save-peer` | `label-save-peer`: `Label save followed a borrowed peer across settings reclaim` |
| `label-delete-peer` | `label-delete-peer`: `Label delete lost its original peer during settings deletion` |
| `unpair-selection` | Both `unpair-selection` and `unpair-replacement`: `Unpair accepted a changed selection or peer after its HCI wait` |

`rename-reclaim` restores the previous borrowed RPC-to-helper path, including
removing the helper's second snapshot defense. The direct-helper controls
restore the borrowed path through the selected save or delete helper.
`unpair-selection` removes only the
post-selection check. No fixture, assertion, schema or dependency is changed.
After a successful compile, require the named assertion in three fresh-process
executions of the exact same binary. The runner fails fast, but `--output`
retains the compiled `standalone-optimized` executable even when its first
execution fails; pass the same case argument when replaying it. A compiler
error, unrelated crash, timeout or missing PASS marker does not qualify.

These hooks establish module behavior at controlled blocking API boundaries.
They do not establish atomicity inside DYA `clear_profile_bond()`: that core
function uses its own mutable peer pointer in `bt_unpair()`, then clears the
captured index without a peer-generation comparison. The core has no public
conditional index/peer clear API or selection epoch. Selection events occur
after advertising updates and cannot supply a complete ABA fence. BLE-1's
unpair and restoration portion therefore remains incomplete; no core changes,
feature restriction or restoration-policy change is made here.

This pins the module RPC payload, not the enclosing Studio transport or a
physical bond/connection. Keep owner registration/build tests and physical
acceptance separate; this regression does not establish reconnection, storage or
reset behavior and does not exercise a real bond-clear operation.
