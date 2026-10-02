#ifndef TINYTAG_CROP_MERGE_H
#define TINYTAG_CROP_MERGE_H
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Execution geometry only: tracking identity never gets merged.
struct DecodeCrop {
    int x, y, width, height;
    bool low;
    std::vector<size_t> members;
    int64_t area() const { return int64_t(width) * height; }
};
inline std::vector<DecodeCrop> merge_decode_crops(std::vector<DecodeCrop> crops, bool enabled)
{
    if (!enabled) return crops;
    // Revisit earlier pairs after each merge, including containment/chains.
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < crops.size() && !changed; ++i)
            for (size_t j = i + 1; j < crops.size(); ++j) {
                const auto &a = crops[i]; const auto &b = crops[j];
                if (a.low != b.low) continue; // do not promote a cheap scan to full resolution
                const int w = std::max(0, std::min(a.x+a.width, b.x+b.width)-std::max(a.x,b.x));
                const int h = std::max(0, std::min(a.y+a.height, b.y+b.height)-std::max(a.y,b.y));
                if (int64_t(w)*h * 2 < std::min(a.area(), b.area())) continue;
                const int x = std::min(a.x,b.x), y = std::min(a.y,b.y);
                const int right = std::max(a.x+a.width,b.x+b.width);
                const int bottom = std::max(a.y+a.height,b.y+b.height);
                // Bounding rectangles can add empty corner regions. Require
                // at least 10% fewer planned pixels, not just high overlap.
                if (int64_t(right-x)*(bottom-y)*10 > (a.area()+b.area())*9) continue;
                DecodeCrop combined{x,y,right-x,bottom-y,a.low,a.members};
                combined.members.insert(combined.members.end(), b.members.begin(), b.members.end());
                std::sort(combined.members.begin(), combined.members.end());
                crops[i] = std::move(combined);
                crops.erase(crops.begin()+j);
                changed = true;
                break;
            }
    }
    return crops;
}
#endif
