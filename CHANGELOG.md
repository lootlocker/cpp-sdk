# Changelog

All notable changes to the LootLocker C++ SDK are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

A deprecated symbol stays for at least one minor release and is removed no earlier than
the next major.

## [Unreleased]

### Added

- Repository scaffolding: the CMake target graph (`LootLocker::core`,
  `LootLocker::auth`, `LootLocker::broadcasts`; no umbrella target), the tiered CI from
  the spec, and the include-path and module-boundary checks.
- `lootlocker::GetVersion()` and `lootlocker::GetVersionString()`.

Nothing is released yet. The first release lands after the MVP features (guest, white
label and Steam auth, plus broadcasts).
