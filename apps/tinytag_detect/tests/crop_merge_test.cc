#include "../crop_merge.h"
#include <cassert>
#include <cstdio>
#include <random>

static DecodeCrop box(int x, int y, int w, int h, bool low, size_t id)
{
    return DecodeCrop{x,y,w,h,low,{id}};
}
static void check_coverage(const std::vector<DecodeCrop> &input)
{
    const auto output = merge_decode_crops(input, true);
    std::vector<unsigned> seen(input.size());
    int64_t before = 0, after = 0;
    for (const auto &a : input) before += a.area();
    for (const auto &a : output) {
        after += a.area();
        for (size_t i : a.members) {
            assert(i < input.size()); ++seen[i];
            const auto &b = input[i];
            assert(a.low == b.low);
            assert(a.x <= b.x && a.y <= b.y);
            assert(a.x+a.width >= b.x+b.width && a.y+a.height >= b.y+b.height);
        }
    }
    for (unsigned count : seen) assert(count == 1);
    assert(after <= before);
    const auto unchanged = merge_decode_crops(input, false);
    assert(unchanged.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        assert(unchanged[i].members == input[i].members);
        assert(unchanged[i].area() == input[i].area());
    }
}
int main()
{
    const auto paired = merge_decode_crops({box(0,0,200,200,false,0),box(50,0,200,200,false,1)},true);
    assert(paired.size() == 1 && paired[0].area() == 50000 && paired[0].members.size() == 2);
    // Same pixels at two policies must retain their separate scans/audits.
    assert(merge_decode_crops({box(0,0,200,200,false,0),box(0,0,200,200,true,1)},true).size() == 2);
    assert(merge_decode_crops({box(0,0,100,100,false,0),box(100,0,100,100,false,1)},true).size() == 2);
    assert(merge_decode_crops({box(0,0,100,100,false,0),box(20,20,10,10,false,1)},true).size() == 2);
    // Half of a long thin crop overlaps, but the bounding union costs more.
    assert(merge_decode_crops({box(0,0,100,100,false,0),box(0,0,200,10,false,1)},true).size() == 2);
    assert(merge_decode_crops({box(0,0,100,100,false,0),box(10,10,80,80,false,1)},true).size() == 1);
    assert(merge_decode_crops({},true).empty());
    std::mt19937 rng(9281);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        std::vector<DecodeCrop> input;
        for (size_t i = 0, n = rng()%16; i < n; ++i)
            input.push_back(box(rng()%1000,rng()%600,8+rng()%280,8+rng()%120,rng()%2,i));
        check_coverage(input);
    }
    puts("PASS: overlap/containment, area inflation, policy separation, rollback and 10000 coverage cases");
}
