#pragma once

// `Optional<T>` (D1). Internal: `lootlocker::core::` is not on an adopter's include path
// (D18).
//
// A thin alias over `std::optional`, kept as our own name so the type can change later
// without a breaking change. D1's C++17 floor is what makes `std::optional` available at
// all; under the superseded C++14 decision this would have been a hand-rolled type.

#include <optional>

namespace lootlocker {
namespace core {

template <typename T>
using Optional = std::optional<T>;

} // namespace core
} // namespace lootlocker
