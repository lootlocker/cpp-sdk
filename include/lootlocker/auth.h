#pragma once

// Placeholder for the auth module's public surface.
//
// T8 scaffolds the target graph only. The real surface lands with T14 (guest), T15
// (white label), T16 (Steam) and T18 (end session + refresh), per
// specs-library/specs/discipline/sdk/cpp-sdk/api-sketch.md.

namespace lootlocker {
namespace auth {

// Intentionally empty until T14. The header exists so the target graph and the
// include-path boundary (D18) can be verified before any feature is written.

} // namespace auth
} // namespace lootlocker
