# AGENTS.md — LootLocker C++ SDK

Rules for AI agents (and humans) working in this repository. Read this before writing any
code. It is the short version of the conventions this codebase enforces; the long version
is the spec, linked at the bottom.

**Never merge a PR.** Branch, open a PR, confirm CI is green, then stop — a human merges.
A green CI run means *ready for review*, not *ready to land*.

**One task fully merged before the next starts.** Do not stack open PRs.

## The spec is the contract

Every non-obvious decision here was made deliberately and recorded. Before changing
behaviour, read the relevant decision. If implementation contradicts the spec, update the
spec in the same effort and say so — never let them drift silently.

## Non-negotiables

These are not style preferences. A change that breaks one is wrong even if it compiles.

- **No exceptions, no RTTI.** The library is built with `/EHs-c- /GR-` and
  `-fno-exceptions -fno-rtti`. Game builds disable them.
- **No internal threads by default.** All callbacks fire on the thread that calls
  `Update()`. The built-in scheduler is opt-in and must never start implicitly.
- **No implicit config.** The SDK reads no file and no environment variable on its own.
  The `Config` struct is the only input. The one documented exception is
  `SystemTimezoneProvider`, which reads the OS timezone because that is the platform's
  answer to "where is this machine" rather than configuration — and an adopter who wants
  to control it injects a `TimezoneProvider` instead.
- **No blocking calls.** `Update()` never waits on the network. The transport is pollable
  (`Start`/`Poll`/`Cancel`), not blocking.
- **No silent defaults on a bad response.** Every undecodable body is
  `ErrorCode::MalformedResponse`. A missing field is a decode error, not a zero.
- **`RawBody()` is populated on every result**, success and failure.
- **No integer ID through a `double`.** `player_id` is `uint64` end to end, enforced by
  `static_assert` on the field type.
- **No public header includes the JSON library.** It lives behind a pimpl.
- **A feature module depends on `core` and on nothing else.** No umbrella target.
- **Only `include/` is on the adopter's include path.** `src/` is unreachable.
- **No platform SDK linkage** in the core library.
- **Generated files are never hand-edited.** `src/core/timezone_table.inc` comes from
  `data/windows-to-iana.tsv` via `tools/generate_timezone_table.py`; CI fails on any
  drift. Change the TSV, then regenerate.

## Naming

`PascalCase` for types and functions, `snake_case` for locals and parameters, `m_` for
class members, `kPascalCase` for constants, `SCREAMING_SNAKE_CASE` for macros only where
unavoidable. Wire structs mirror the JSON exactly, so their fields are `snake_case`.

There is **no accessor exception**: `Session()` and `RawBody()`, not `session()` and
`raw_body()`. One rule for functions, no exceptions.

Functions that do work are **verb-named** (`StartGuestSession`, not `guest_session`).
Async is marked by the signature — a callback parameter and a `RequestId` return — never
by a name suffix.

## Async

One rule, and it is one sentence: **a callback is for the result of a call you made; the
event system is for things that happen to you.** Nothing appears in both.

## Layout

```
include/lootlocker/   public headers — the only thing an adopter can include
src/core/             internal machinery: json, executor, transport, state
src/auth/             the auth module
src/broadcasts/       the broadcasts module
data/                 checked-in source data (the timezone table's TSV)
tools/                generators that turn data/ into src/ (never run by a normal build)
tests/                tier 1 unit tests and the tier 2 consumer
examples/             runnable examples, not part of the library
third_party/          vendored dependencies, as submodules
```

`src/` is not on the adopter's include path. A public header that includes a private one
breaks the contract even though it compiles in-tree, and CI fails on it.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`third_party/` holds submodules, so a fresh clone needs `--recursive` (or
`git submodule update --init --recursive`).

Two options matter when you are not just building:

- `-DLOOTLOCKER_MODULE_ONLY="auth;broadcasts"` declares only those feature modules. It is
  a list; one module is a list of one. **The value must be quoted** on a POSIX shell, or
  the `;` ends the `cmake` command and `broadcasts` runs as a separate one. This is how
  the module-boundary CI proves isolation, and it is how a game names the modules it uses.
- `-DLOOTLOCKER_REGENERATE_TIMEZONE=ON` regenerates `src/core/timezone_table.inc` from
  `data/windows-to-iana.tsv`. It needs Python, which is why it is off by default — an
  adopter must never need the generator. Packaging turns it on.

## Branching

Feature branches come off `dev` and are **rebased** onto it, never merged into it. `main`
is releases only. Every commit must build and represent one logical change; no WIP or
fixup commits in the history.

## The spec

- [Spec — decisions, risks, rationale](https://github.com/lootlocker/index/blob/main/specs-library/specs/discipline/sdk/cpp-sdk/README.md)
- [API sketch — the public surface](https://github.com/lootlocker/index/blob/main/specs-library/specs/discipline/sdk/cpp-sdk/api-sketch.md)
- [Tasks — what is being built and in what order](https://github.com/lootlocker/index/blob/main/specs-library/specs/discipline/sdk/cpp-sdk/tasks.md)
