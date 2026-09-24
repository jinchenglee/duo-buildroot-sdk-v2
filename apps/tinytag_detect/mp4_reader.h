// Minimal H.264 MP4 demuxer, header-only, for --input playback through VDEC.
// The counterpart of mp4_writer.h: reads the first video track's sample table
// and returns each sample as an Annex-B access unit (start codes instead of
// length prefixes, with the avcC SPS/PPS prepended to every sync sample), which
// is what CVI_VDEC_SendStream expects in VIDEO_MODE_FRAME. Handles files with
// the moov box before or after mdat, 32- or 64-bit chunk offsets, and any stsc
// layout. H.265 and fragmented MP4 are rejected.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class Mp4Reader
{
public:
    ~Mp4Reader()
    {
        if (fp_)
            std::fclose(fp_);
    }

    bool open(const std::string &path, std::string &error)
    {
        fp_ = std::fopen(path.c_str(), "rb");
        if (!fp_)
            return fail(error, "cannot open " + path);

        Buf moov;
        uint64_t pos = 0;
        for (;;)
        {
            uint8_t hdr[16];
            if (!read_at(pos, hdr, 8))
                break;
            uint64_t size = be32(hdr);
            uint32_t header = 8;
            if (size == 1)
            {
                if (!read_at(pos + 8, hdr + 8, 8))
                    break;
                size = be64(hdr + 8);
                header = 16;
            }
            else if (size == 0)
                break; // box runs to end of file; moov cannot follow it
            if (size < header)
                return fail(error, "corrupt top-level box");
            if (tag_is(hdr + 4, "moov"))
            {
                moov.resize(size - header);
                if (!read_at(pos + header, moov.data(), moov.size()))
                    return fail(error, "truncated moov box");
                break;
            }
            if (tag_is(hdr + 4, "moof"))
                return fail(error, "fragmented MP4 is not supported");
            pos += size;
        }
        if (moov.empty())
            return fail(error, "no moov box (file not finalized?)");

        for (const Box &trak : children(moov, 0, moov.size(), "trak"))
        {
            if (parse_trak(moov, trak, error))
                return true;
            if (!error.empty())
                return false;
        }
        return fail(error, "no H.264 video track");
    }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    size_t frame_count() const { return samples_.size(); }
    // True when the track has composition offsets, i.e. B-frame reordering.
    bool reorders() const { return reorders_; }
    double fps() const
    {
        if (samples_.size() < 2 || samples_.back().dts == 0)
            return 0.0;
        return (samples_.size() - 1) * static_cast<double>(timescale_) / samples_.back().dts;
    }

    // Sample `index` as an Annex-B access unit. `dts_us` is the decode
    // timestamp relative to the first sample.
    bool read_frame(size_t index, std::vector<uint8_t> &out, uint64_t &dts_us, bool &key)
    {
        if (index >= samples_.size())
            return false;
        const Sample &s = samples_[index];
        raw_.resize(s.size);
        if (!read_at(s.offset, raw_.data(), raw_.size()))
            return false;
        dts_us = s.dts * 1000000ull / timescale_;
        key = s.key;

        out.clear();
        static const uint8_t kStart[4] = {0, 0, 0, 1};
        if (key)
        {
            for (const Buf &ps : parameter_sets_)
            {
                out.insert(out.end(), kStart, kStart + 4);
                out.insert(out.end(), ps.begin(), ps.end());
            }
        }
        size_t p = 0;
        while (p + nal_length_size_ <= raw_.size())
        {
            uint32_t len = 0;
            for (uint32_t i = 0; i < nal_length_size_; ++i)
                len = (len << 8) | raw_[p + i];
            p += nal_length_size_;
            if (len > raw_.size() - p)
                return false;
            out.insert(out.end(), kStart, kStart + 4);
            out.insert(out.end(), raw_.begin() + p, raw_.begin() + p + len);
            p += len;
        }
        return !out.empty();
    }

private:
    using Buf = std::vector<uint8_t>;
    struct Box
    {
        size_t body; // offset of the payload inside the parent buffer
        size_t end;
    };
    struct Sample
    {
        uint64_t offset = 0;
        uint32_t size = 0;
        uint64_t dts = 0; // in track timescale units, first sample = 0
        bool key = false;
    };

    static bool fail(std::string &error, const std::string &message)
    {
        error = message;
        return false;
    }
    static uint32_t be16(const uint8_t *p) { return (uint32_t(p[0]) << 8) | p[1]; }
    static uint32_t be32(const uint8_t *p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    static uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
    static bool tag_is(const uint8_t *p, const char *t)
    {
        return p[0] == uint8_t(t[0]) && p[1] == uint8_t(t[1]) && p[2] == uint8_t(t[2]) &&
               p[3] == uint8_t(t[3]);
    }

    bool read_at(uint64_t pos, uint8_t *dst, size_t len)
    {
        if (std::fseek(fp_, static_cast<long>(pos), SEEK_SET) != 0)
            return false;
        return std::fread(dst, 1, len, fp_) == len;
    }

    // Direct children of type `type` within [begin, end) of `b`.
    static std::vector<Box> children(const Buf &b, size_t begin, size_t end, const char *type)
    {
        std::vector<Box> out;
        size_t p = begin;
        while (p + 8 <= end)
        {
            uint64_t size = be32(&b[p]);
            size_t header = 8;
            if (size == 1 && p + 16 <= end)
            {
                size = be64(&b[p + 8]);
                header = 16;
            }
            else if (size == 0)
                size = end - p;
            if (size < header || size > end - p)
                break;
            if (tag_is(&b[p + 4], type))
                out.push_back(Box{p + header, static_cast<size_t>(p + size)});
            p += static_cast<size_t>(size);
        }
        return out;
    }
    static bool child(const Buf &b, const Box &parent, const char *type, Box &out)
    {
        const std::vector<Box> found = children(b, parent.body, parent.end, type);
        if (found.empty())
            return false;
        out = found.front();
        return true;
    }

    // Returns false with an empty `error` for a track that is simply not
    // H.264 video, so the caller moves on to the next one.
    bool parse_trak(const Buf &b, const Box &trak, std::string &error)
    {
        Box mdia{}, hdlr{}, mdhd{}, minf{}, stbl{}, stsd{};
        if (!child(b, trak, "mdia", mdia) || !child(b, mdia, "hdlr", hdlr) ||
            hdlr.end - hdlr.body < 12 || !tag_is(&b[hdlr.body + 8], "vide"))
            return false;
        if (!child(b, mdia, "mdhd", mdhd) || !child(b, mdia, "minf", minf) ||
            !child(b, minf, "stbl", stbl) || !child(b, stbl, "stsd", stsd))
            return fail(error, "incomplete video track");

        const bool v1 = b[mdhd.body] == 1;
        timescale_ = be32(&b[mdhd.body + (v1 ? 20 : 12)]);
        if (timescale_ == 0)
            return fail(error, "video track has zero timescale");

        // stsd: version/flags, entry count, then the first sample entry.
        const size_t entry = stsd.body + 8;
        if (entry + 8 + 78 > stsd.end)
            return fail(error, "truncated sample description");
        const uint8_t *type = &b[entry + 4];
        if (tag_is(type, "hvc1") || tag_is(type, "hev1"))
            return fail(error, "H.265 input is not supported; re-encode to H.264");
        if (!tag_is(type, "avc1") && !tag_is(type, "avc3"))
            return fail(error, "unsupported video codec (need H.264 avc1)");
        const size_t fields = entry + 8;
        width_ = be16(&b[fields + 24]);
        height_ = be16(&b[fields + 26]);
        const Box entry_box{fields + 78, entry + be32(&b[entry])};
        Box avcc{};
        if (!child(b, entry_box, "avcC", avcc) || avcc.end - avcc.body < 7)
            return fail(error, "H.264 track without avcC");
        // The hardware decoder is 8-bit 4:2:0 only: High 10, High 4:2:2,
        // High 4:4:4 and CAVLC 4:4:4 streams would decode to nothing.
        const unsigned profile = b[avcc.body + 1];
        if (profile == 110 || profile == 122 || profile == 244 || profile == 44)
            return fail(error, "H.264 profile " + std::to_string(profile) +
                                   " (10-bit, 4:2:2 or 4:4:4) is not supported by the hardware"
                                   " decoder; re-encode with -pix_fmt yuv420p");
        if (!parse_avcc(b, avcc))
            return fail(error, "corrupt avcC");

        return build_samples(b, stbl, error);
    }

    bool parse_avcc(const Buf &b, const Box &avcc)
    {
        size_t p = avcc.body + 4;
        nal_length_size_ = (b[p++] & 3) + 1;
        for (int kind = 0; kind < 2; ++kind) // SPS, then PPS
        {
            if (p >= avcc.end)
                return false;
            const unsigned count = kind == 0 ? (b[p++] & 0x1f) : b[p++];
            for (unsigned i = 0; i < count; ++i)
            {
                if (p + 2 > avcc.end)
                    return false;
                const size_t len = be16(&b[p]);
                p += 2;
                if (p + len > avcc.end)
                    return false;
                parameter_sets_.emplace_back(b.begin() + p, b.begin() + p + len);
                p += len;
            }
        }
        return !parameter_sets_.empty();
    }

    bool build_samples(const Buf &b, const Box &stbl, std::string &error)
    {
        Box stsz{}, stsc{}, stts{}, stco{}, stss{};
        const bool co64 = !child(b, stbl, "stco", stco) && child(b, stbl, "co64", stco);
        if (!child(b, stbl, "stsz", stsz) || !child(b, stbl, "stsc", stsc) ||
            !child(b, stbl, "stts", stts) || stco.end == 0)
            return fail(error, "incomplete sample table");

        const uint32_t uniform = be32(&b[stsz.body + 4]);
        const uint32_t count = be32(&b[stsz.body + 8]);
        if (uniform == 0 && stsz.body + 12 + 4ull * count > stsz.end)
            return fail(error, "truncated stsz");
        samples_.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            samples_[i].size = uniform ? uniform : be32(&b[stsz.body + 12 + 4 * i]);

        // Chunk offsets, then walk stsc runs to place samples in chunks.
        const uint32_t chunks = be32(&b[stco.body + 4]);
        const size_t step = co64 ? 8 : 4;
        if (stco.body + 8 + step * chunks > stco.end)
            return fail(error, "truncated chunk offsets");
        const uint32_t runs = be32(&b[stsc.body + 4]);
        if (stsc.body + 8 + 12ull * runs > stsc.end)
            return fail(error, "truncated stsc");
        uint32_t sample = 0;
        for (uint32_t r = 0; r < runs && sample < count; ++r)
        {
            const uint8_t *run = &b[stsc.body + 8 + 12 * r];
            const uint32_t first = be32(run);
            const uint32_t per_chunk = be32(run + 4);
            const uint32_t last = r + 1 < runs ? be32(run + 12) - 1 : chunks;
            for (uint32_t c = first; c <= last && c >= 1 && c <= chunks && sample < count; ++c)
            {
                const uint8_t *o = &b[stco.body + 8 + step * (c - 1)];
                uint64_t offset = co64 ? be64(o) : be32(o);
                for (uint32_t k = 0; k < per_chunk && sample < count; ++k)
                {
                    samples_[sample].offset = offset;
                    offset += samples_[sample].size;
                    ++sample;
                }
            }
        }
        if (sample != count)
            return fail(error, "sample table does not cover every sample");

        // Decode timestamps.
        const uint32_t deltas = be32(&b[stts.body + 4]);
        uint64_t dts = 0;
        sample = 0;
        for (uint32_t r = 0; r < deltas && stts.body + 16 + 8 * r <= stts.end; ++r)
        {
            const uint32_t n = be32(&b[stts.body + 8 + 8 * r]);
            const uint32_t delta = be32(&b[stts.body + 12 + 8 * r]);
            for (uint32_t k = 0; k < n && sample < count; ++k)
            {
                samples_[sample++].dts = dts;
                dts += delta;
            }
        }

        Box ctts{};
        reorders_ = child(b, stbl, "ctts", ctts);

        // Sync samples; a track without stss is all sync samples.
        if (child(b, stbl, "stss", stss))
        {
            const uint32_t n = be32(&b[stss.body + 4]);
            for (uint32_t i = 0; i < n && stss.body + 12 + 4 * i <= stss.end; ++i)
            {
                const uint32_t idx = be32(&b[stss.body + 8 + 4 * i]);
                if (idx >= 1 && idx <= count)
                    samples_[idx - 1].key = true;
            }
        }
        else
        {
            for (auto &s : samples_)
                s.key = true;
        }
        if (samples_.empty())
            return fail(error, "video track has no samples");
        return true;
    }

    std::FILE *fp_ = nullptr;
    uint32_t width_ = 0, height_ = 0, timescale_ = 0, nal_length_size_ = 4;
    bool reorders_ = false;
    std::vector<Buf> parameter_sets_;
    std::vector<Sample> samples_;
    Buf raw_;
};
