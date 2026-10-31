# Owner tests

This repository owns its executable test logic and fixtures under `tests/`.

`tests/web/lifecycle.spec.tsx` adds eighteen actual-provider scenarios across
Profile, Output Priority and Split panels. Each panel preserves connection/index
ownership for late load success/error, old completion versus a new refresh,
mutation success/error and follow-up reads, and unmount. Fifteen cases fail on
the archived pre-repair application; the three unmount boundaries and all
seventeen original scenarios remain. All visible state, including profile
editing, belongs to the current panel instance. An older operation cannot issue
a follow-up read after its connection has retired. `npm test`, watch and coverage
generate the current owner schema before collection, including a clean checkout
where generated TypeScript is absent. No dependency install is implicit.

`tests/web/splitRetry.spec.tsx` adds six actual-provider recovery scenarios.
An initial transport rejection, the real SDK timeout and a decoded RPC error
must expose the current error and a same-connection Refresh action. Recovery
remains visible and disabled while a retry is pending; a failed retry can be
retried again. Connection replacement resets recovery and retires an old retry
error. Initial unknown state and successful non-split replies stay hidden,
including a non-split result after recovery. Five cases fail on the targeted
pre-repair component; the original initial-state assertions remain unchanged.

`tests/web/splitResponseBoundary.spec.tsx` adds five provider cases. An absent
custom payload, empty response, unrelated response kind or split response
without its info must show the current error and allow same-connection retry.
All four fail on the component before this response-validation repair. A late
absent-payload reply from a retired connection remains a passing control and
cannot expose an error or finish the new connection's pending read. Valid
non-split responses retain the separate hiding assertions above.

`tests/web/panelOwnership.spec.tsx` adds six same-connection subsystem-index
replacement cases across all three panels, retaining reset/data/error/loading
ownership. Ten profile mutation cases cover Unpair and Save name success,
encoded RPC errors and rejection, loading through the follow-up read, and
retired success/error without a follow-up after connection replacement.
These twenty-two cases add to all thirty-five previous Web assertions; the
request-lifetime helper and original fixtures are unchanged. The tests use the
actual provider/service/schema with a controlled call_rpc boundary; they do not
establish physical transport or cancel already admitted device operations.

## Current test ownership and compatibility

This owner supports DYA custom Studio RPC. Stock ZMK is NOT APPLICABLE;
no feature-disabled stock check is required or counted as compatibility.

Canonical entries (run from the repository root):

```bash
python3 -m unittest discover -v -s tests -p test_owner.py
bash tests/run-web.sh
```

[maintenance.json](maintenance.json) records supported cores, current commands
and explicit gaps. It is a reference map, not a passing test or a claim that
registration/coverage totals establish completeness. Preserve ordered outputs,
raw timestamps and the first failure. Read actual dependencies before execution.

`OWNER_TEST_OUTPUT=/absolute/new/directory` retains owner native/build outputs.
An existing directory fails before tests. Without it a fresh temporary directory
is printed and retained. No shared build directory is deleted. Use
`PYTHONPATH=tests python3 -m unittest -v test_owner.WestCommandsTests.<method>`
for a focused case; discovery runs the complete documented class.

Web assertions and Jest configuration live in `tests/web/`; application source
and installed dependencies remain in `web/`. The package scripts only dispatch
the owner configuration. No dependencies are installed implicitly by the tests.

The native verifier runs each built binary three times in independent empty
initial-flash directories, retaining raw output before filtering. It checks real
process exits, fatal assertions and every linked checked-init result before
comparing unchanged owner snapshots. A SYS_INIT error cannot be hidden by a
later matching key trace. Native scheduling and controlled APIs still do not
establish physical transport/retention. Test-only core/tool dependencies are
pinned; update pins only after reviewing the new sources and rerunning this
owner matrix. No network updates are implicit in the owner test command.

## Reproducible isolated checks

Use this owner's `west/west-test-standalone.yml` and its pinned test dependency
manifest. Test ZMK is `e5c9b6915b56801193e359dd9bad4a167ce0d1b8`;
Zephyr is `10ba6d0cb38bc3d258775d27982f707599320085`. Resolve the
manifest and verify actual checkout identities before running. Do not inherit
global CMake module overrides or moving production dependency references into
these isolated fixtures. The CI SDK image is pinned by digest in
`.github/workflows/zmk-module.yml`; production dependency manifests are separate.

Use a new `OWNER_TEST_OUTPUT` directory for every complete owner run. Native
repeats reuse one compiled binary with independent initial-flash state; keep
the first failure and raw combined stdout/stderr order. DYA-only compatibility
still means no stock-ZMK test requirement.

The owner command boundary limits `zmk-build` to one target at a time by
default. `OWNER_TEST_BUILD_PARALLELISM` selects an explicit positive target
count; account for compiler jobs and concurrent owners in the global budget.
`tests/test_build_budget.py` checks default/explicit forwarding and rejects
invalid budgets before launching west. The canonical owner entry includes
these controls without dropping any native or board scenario.

Native result admission requires every `native_sim.keymap` fixture to have a
matching binary. `required_hooks.json` names the expected checked initialization
hooks and required feature/role configuration. Fixture-bound declarations also
reject missing configuration when both the request and build omit it; excluded
roles cannot replace the requested role. The verifier retains all three raw
runs before comparing unchanged ordered snapshots and rejects logged failures,
fatal output and missing/nonzero completions. `tests/test_native_verifier.py`
owns eleven real-ELF admission controls, including a deliberately missing
native scenario, wrong-role and unrelated-fixture controls.

`bash tests/run-web.sh` is the canonical schema-generation/Web test entry.
It runs all configured Jest scenarios in one worker process. The pre-commit
Jest hook dispatches this same owner entry so nested workers do not multiply
the global container budget. Build and lint retain their separate package entries.
