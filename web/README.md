# BLE Management Web UI

This React client manages keyboard BLE profiles, profile labels, output
preference and split connection information through the `cormoran_ble` custom
Studio RPC subsystem. The current standalone connection screen offers **Connect
via Serial**. Managing BLE profiles does not require the browser to connect
through BLE; use the keyboard's Studio serial interface.

The keyboard must enable this module and DYA custom Studio RPC. Firmware
permission checks still apply to each request. A successful connection does not
establish that a profile change or split operation reached a physical peer.

## Current sources

- [App.tsx](src/App.tsx) owns the Serial connection controls and subsystem ID.
- `src/components/` contains the profile, split and output-preference views.
- The wire schema is
  [ble_management.proto](../proto/zmk/ble_management/ble_management.proto).
- `npm run generate` uses `buf.gen.yaml` to generate ignored TypeScript messages
  under `src/proto/zmk/ble_management/`. Do not edit generated files by hand.
- Executable UI assertions and Jest configuration live in `../tests/web/`.
  Application sources and installed packages remain in `web/`.

## Development and verification

Use the checked-in npm lockfile when preparing dependencies. From the owner root:

```sh
# Explicit dependency preparation, when needed
cd web
npm ci
npm run generate
npm run dev
```

Run the canonical Web checks from the owner root after preparing dependencies:

```sh
bash tests/run-web.sh
```

From `web/`, tests, watch and coverage generate the current schema before Jest
collection. `npm run build` generates types, checks TypeScript and builds the
client. `npm run lint` checks the application and owner test files. Each panel's
state belongs to its connection and subsystem; late request results, errors,
completion and mutation follow-up reads retire with that source. Tests use
controlled connection/RPC helpers; they do not establish physical USB/BLE/split,
bond storage, radio delivery or access-control behavior on a board. See the
[owner test guide](../tests/README.md) for the separate firmware checks.

Every operation admits a single matching response. Absent payloads, empty or
unrelated responses, and mixed oneof members are errors; blank RPC errors get a
visible fallback. Mutation replies need their explicit `success` flag. An empty
nested mutation message defaults to false and cannot trigger a success refresh,
retire a name draft or display a pairing-clear confirmation. A failed request
can be retried on the same connection.

An initial split-status failure shows its error and a Refresh action without
requiring a reconnect. The recovery view remains visible while retrying and
retires with its connection/subsystem. Initial unknown state and a confirmed
non-split keyboard remain hidden.
Missing payloads and responses without split information are retrieval errors
and expose the same retry action.

Building local output does not publish the application.

## Console privacy

Console failures include a fixed operation label and an allowlisted error category.
Messages, stacks, causes, arbitrary error names, profile/device names, addresses,
setting keys, prefixes, and values are not console arguments. UI errors and
intentional profile displays retain their existing behavior. Individual RPC
payloads and notifications are not logged.
The application installs its console guard before importing the app and Studio
SDKs. The owner tests inventory executable console calls in both locked SDKs and
require reviewed fixed first labels; changed or new calls stop the tests for
review. The same runtime guard applies in development and production.
Browser-generated diagnostics and unrelated browser extensions are outside this
application's control.

## Dependency security

`package.json` overrides the Studio client's transitive `protobufjs` dependency
to `7.6.5`, including the patched UTF-8 helper. Use `npm ci` with this lockfile.
After changing dependencies, run `npm audit --omit=dev`, the Web tests and the
production build.

The development lock also includes compatible patches such as Handlebars
`4.7.10`, without changing the direct dependency requirements or the Studio
client commits. On 2026-11-07 the full lock audit reports 18 development-only
findings (four low, fourteen moderate; no critical or high), while
`npm audit --omit=dev` reports zero. Full audit exits nonzero: the remaining
paths include `deepmerge` with no published patch, and fixes for `glob` and
`minimatch` outside their incoming major-version ranges. These advisory results
do not establish an application exploit. Retain the full and production-only
audit outputs when rechecking; the production-only result does not clear the
development tools.
