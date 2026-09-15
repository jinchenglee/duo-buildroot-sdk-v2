#include "golden.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

constexpr char kMagic[8] = {'T', 'T', 'G', 'O', 'L', 'D', '0', '1'};
constexpr size_t kNameLen = 64;

void read_exact(FILE *file, void *destination, size_t bytes, const char *what)
{
    if (std::fread(destination, 1, bytes, file) != bytes)
        throw std::runtime_error(std::string("golden bundle truncated while reading ") + what);
}

} // namespace

GoldenBundle load_golden(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        throw std::runtime_error("cannot open golden bundle: " + path);

    // Guarantees the file is closed on any throw below.
    struct Closer
    {
        FILE *f;
        ~Closer() { std::fclose(f); }
    } closer{file};

    char magic[sizeof(kMagic)];
    read_exact(file, magic, sizeof(magic), "magic");
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        throw std::runtime_error(path + " is not a TTGOLD01 bundle");

    uint32_t counts[6];
    read_exact(file, counts, sizeof(counts), "header");

    GoldenBundle bundle;
    const uint32_t frame_count = counts[0];
    bundle.in_h = counts[1];
    bundle.in_w = counts[2];
    bundle.out_c = counts[3];
    bundle.out_h = counts[4];
    bundle.out_w = counts[5];

    if (frame_count == 0 || bundle.input_count() == 0 || bundle.output_count() == 0)
        throw std::runtime_error("golden bundle header describes an empty tensor");

    bundle.frames.reserve(frame_count);
    for (uint32_t i = 0; i < frame_count; ++i)
    {
        GoldenFrame frame;

        char name[kNameLen];
        read_exact(file, name, sizeof(name), "frame name");
        name[kNameLen - 1] = '\0';
        frame.name = name;

        frame.input.resize(bundle.input_count());
        read_exact(file, frame.input.data(), frame.input.size(), "frame input");

        frame.fp32.resize(bundle.output_count());
        read_exact(file, frame.fp32.data(), frame.fp32.size() * sizeof(float), "fp32 reference");

        frame.sim8.resize(bundle.output_count());
        read_exact(file, frame.sim8.data(), frame.sim8.size() * sizeof(float), "simulator reference");

        bundle.frames.push_back(std::move(frame));
    }

    return bundle;
}
