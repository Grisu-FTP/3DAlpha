#include "core/preview/preview_window.hpp"

namespace mc::preview {

int wantedOrder(int cursor, int count, int radius, int* out, int max)
{
    int written = 0;
    if (count <= 0 || out == nullptr || max <= 0) {
        return 0;
    }
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor >= count) {
        cursor = count - 1;
    }
    out[written++] = cursor;
    for (int step = 1; step <= radius && written < max; ++step) {
        if (cursor + step < count) {
            out[written++] = cursor + step;
        }
        if (written < max && cursor - step >= 0) {
            out[written++] = cursor - step;
        }
    }
    return written;
}

int farthestOutside(const int* resident, int count, int cursor, int radius)
{
    int best = -1;
    int bestDistance = radius;
    for (int i = 0; i < count; ++i) {
        if (resident[i] < 0) {
            continue;
        }
        int distance = resident[i] - cursor;
        if (distance < 0) {
            distance = -distance;
        }
        if (distance > bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

}  // namespace mc::preview
