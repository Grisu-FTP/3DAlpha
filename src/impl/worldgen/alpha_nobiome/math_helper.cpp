#include "impl/worldgen/alpha_nobiome/math_helper.hpp"

#include <cmath>

namespace mc::worldgen {

float* MathHelper::table_ = nullptr;
bool MathHelper::built_ = false;

void MathHelper::ensureBuilt()
{
    if (built_) {
        return;
    }
    // Never freed, and deliberately: it is one allocation for the life of the
    // process, the same 256 KB whenever it is asked for, and handing it back
    // would only give the heap a hole to fragment around.
    table_ = new float[65536];
    for (int i = 0; i < 65536; ++i) {
        // The original's expression, in the original's order: i * PI, then * 2,
        // then / 65536. Reassociating it -- (i * 2 * PI) / 65536, or
        // i * (2*PI/65536) -- changes the last bits of some entries, and those
        // bits reach the cave carver.
        table_[i] = float(std::sin(double(i) * 3.141592653589793 * 2.0 / 65536.0));
    }
    built_ = true;
}

const float* MathHelper::table()
{
    ensureBuilt();
    return table_;
}

}  // namespace mc::worldgen
