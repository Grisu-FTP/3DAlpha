#pragma once

// Binds the `items` slot for versions whose inventory is 36 main slots plus
// four armour slots written at index + 100. Included by the generated
// version_slots.hpp; see docs/build-versions.md.

#include "impl/items/b1_2/layout.hpp"

namespace mcver {

using ItemLayout = mc::items::B1_2Layout;

}  // namespace mcver
