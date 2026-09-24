// Minimal single-track H.264 MP4 muxer, header-only, for on-board recording.
// The board has no ffmpeg, so the encoder's Annex-B packs are converted to
// length-prefixed samples here and the moov box is written when the file is
// closed. Not crash-safe: a file that is never close()d has no moov and will
// not play.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

class Mp4Writer
{
public:
    ~Mp4Writer() { close(); }

    bool open(const std::string &path, uint32_t width, uint32_t height)
    {
        width_ = width;
        height_ = height;
        fp_ = std::fopen(path.c_str(), "wb");
        if (!fp_)
            return false;
        static const uint8_t ftyp[] = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
                                       'i', 's', 'o', 'm', 'a', 'v', 'c', '1'};
        std::fwrite(ftyp, 1, sizeof(ftyp), fp_);
        mdat_pos_ = sizeof(ftyp);
        const uint8_t mdat[] = {0, 0, 0, 0, 'm', 'd', 'a', 't'};
        std::fwrite(mdat, 1, sizeof(mdat), fp_);
        offset_ = mdat_pos_ + 8;
        return true;
    }

    bool is_open() const { return fp_ != nullptr; }
    size_t frames() const { return samples_.size(); }

    // `data` is the whole access unit in Annex-B form. Frames before the first
    // SPS/PPS-bearing IDR are dropped. Returns false if the frame was dropped.
    bool add_frame(const uint8_t *data, size_t len, uint64_t pts_us)
    {
        if (!fp_)
            return false;
        nals_.clear();
        split_nals(data, len);
        bool key = false;
        for (const auto &n : nals_)
        {
            const int type = data[n.first] & 0x1f;
            if (type == 7 && sps_.empty())
                sps_.assign(data + n.first, data + n.first + n.second);
            else if (type == 8 && pps_.empty())
                pps_.assign(data + n.first, data + n.first + n.second);
            else if (type == 5)
                key = true;
        }
        if (sps_.empty() || pps_.empty() || (samples_.empty() && !key))
            return false;

        Sample s;
        s.offset = offset_;
        s.key = key;
        s.pts_us = pts_us;
        for (const auto &n : nals_)
        {
            const int type = data[n.first] & 0x1f;
            if (type == 7 || type == 8 || type == 9) // parameter sets live in avcC
                continue;
            uint8_t hdr[4] = {uint8_t(n.second >> 24), uint8_t(n.second >> 16), uint8_t(n.second >> 8),
                              uint8_t(n.second)};
            std::fwrite(hdr, 1, 4, fp_);
            std::fwrite(data + n.first, 1, n.second, fp_);
            s.size += 4 + static_cast<uint32_t>(n.second);
        }
        offset_ += s.size;
        samples_.push_back(s);
        return true;
    }

    void close()
    {
        if (!fp_)
            return;
        if (!samples_.empty() && offset_ < 0xFFFFFFFFull)
        {
            std::vector<uint8_t> moov = build_moov();
            std::fwrite(moov.data(), 1, moov.size(), fp_);
            std::fseek(fp_, static_cast<long>(mdat_pos_), SEEK_SET);
            const uint32_t size = static_cast<uint32_t>(offset_ - mdat_pos_);
            const uint8_t hdr[4] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8),
                                    uint8_t(size)};
            std::fwrite(hdr, 1, 4, fp_);
        }
        std::fclose(fp_);
        fp_ = nullptr;
    }

private:
    struct Sample
    {
        uint64_t offset = 0;
        uint32_t size = 0;
        bool key = false;
        uint64_t pts_us = 0;
    };
    using Buf = std::vector<uint8_t>;
    static constexpr uint32_t kTimescale = 90000;

    static void u16(Buf &b, uint32_t v) { b.push_back(v >> 8); b.push_back(v); }
    static void u32(Buf &b, uint32_t v) { u16(b, v >> 16); u16(b, v & 0xffff); }
    static void tag(Buf &b, const char *t) { b.insert(b.end(), t, t + 4); }
    static void zeros(Buf &b, size_t n) { b.insert(b.end(), n, 0); }
    // Wrap `body` as a box.
    static Buf box(const char *t, const Buf &body)
    {
        Buf b;
        u32(b, static_cast<uint32_t>(body.size() + 8));
        tag(b, t);
        b.insert(b.end(), body.begin(), body.end());
        return b;
    }
    static void append(Buf &dst, const Buf &src) { dst.insert(dst.end(), src.begin(), src.end()); }

    void split_nals(const uint8_t *d, size_t n)
    {
        size_t i = 0;
        long start = -1;
        while (i + 3 <= n)
        {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            {
                if (start >= 0)
                    push_nal(d, start, i);
                start = static_cast<long>(i + 3);
                i += 3;
            }
            else
                ++i;
        }
        if (start >= 0)
            push_nal(d, start, n);
    }
    // Trailing zero bytes are the leading zero of the next 4-byte start code
    // (a NAL unit never legitimately ends in 0x00).
    void push_nal(const uint8_t *d, long start, size_t end)
    {
        size_t e = end;
        while (e > static_cast<size_t>(start) && d[e - 1] == 0)
            --e;
        if (e > static_cast<size_t>(start))
            nals_.emplace_back(static_cast<size_t>(start), e - static_cast<size_t>(start));
    }

    std::vector<uint32_t> durations() const
    {
        std::vector<uint32_t> d(samples_.size(), 0);
        for (size_t i = 0; i + 1 < samples_.size(); ++i)
        {
            const int64_t dt = static_cast<int64_t>(samples_[i + 1].pts_us) - static_cast<int64_t>(samples_[i].pts_us);
            d[i] = dt > 0 ? static_cast<uint32_t>(dt * kTimescale / 1000000) : 1;
        }
        if (samples_.size() > 1)
            d.back() = d[d.size() - 2];
        else
            d.back() = kTimescale / 30;
        return d;
    }

    Buf build_moov() const
    {
        const std::vector<uint32_t> dur = durations();
        uint64_t total = 0;
        for (uint32_t d : dur)
            total += d;
        const uint32_t total32 = static_cast<uint32_t>(total);

        // stsd / avc1 / avcC
        Buf avcc;
        avcc.push_back(1);
        avcc.push_back(sps_[1]);
        avcc.push_back(sps_[2]);
        avcc.push_back(sps_[3]);
        avcc.push_back(0xff); // 4-byte NAL length
        avcc.push_back(0xe1); // one SPS
        u16(avcc, static_cast<uint32_t>(sps_.size()));
        append(avcc, sps_);
        avcc.push_back(1); // one PPS
        u16(avcc, static_cast<uint32_t>(pps_.size()));
        append(avcc, pps_);

        Buf avc1;
        zeros(avc1, 6);
        u16(avc1, 1); // data reference index
        zeros(avc1, 16);
        u16(avc1, width_);
        u16(avc1, height_);
        u32(avc1, 0x00480000);
        u32(avc1, 0x00480000);
        u32(avc1, 0);
        u16(avc1, 1); // frame count
        zeros(avc1, 32); // compressor name
        u16(avc1, 0x18);
        u16(avc1, 0xffff);
        append(avc1, box("avcC", avcc));
        Buf stsd;
        u32(stsd, 0);
        u32(stsd, 1);
        append(stsd, box("avc1", avc1));

        // stts: run-length of durations
        Buf stts;
        u32(stts, 0);
        Buf runs;
        uint32_t nruns = 0;
        for (size_t i = 0; i < dur.size();)
        {
            size_t j = i;
            while (j < dur.size() && dur[j] == dur[i])
                ++j;
            u32(runs, static_cast<uint32_t>(j - i));
            u32(runs, dur[i]);
            ++nruns;
            i = j;
        }
        u32(stts, nruns);
        append(stts, runs);

        Buf stss;
        u32(stss, 0);
        Buf keys;
        uint32_t nkeys = 0;
        for (size_t i = 0; i < samples_.size(); ++i)
            if (samples_[i].key)
            {
                u32(keys, static_cast<uint32_t>(i + 1));
                ++nkeys;
            }
        u32(stss, nkeys);
        append(stss, keys);

        Buf stsc;
        u32(stsc, 0);
        u32(stsc, 1);
        u32(stsc, 1); // first chunk
        u32(stsc, 1); // samples per chunk
        u32(stsc, 1);

        Buf stsz;
        u32(stsz, 0);
        u32(stsz, 0);
        u32(stsz, static_cast<uint32_t>(samples_.size()));
        for (const auto &s : samples_)
            u32(stsz, s.size);

        Buf stco;
        u32(stco, 0);
        u32(stco, static_cast<uint32_t>(samples_.size()));
        for (const auto &s : samples_)
            u32(stco, static_cast<uint32_t>(s.offset));

        Buf stbl;
        append(stbl, box("stsd", stsd));
        append(stbl, box("stts", stts));
        append(stbl, box("stss", stss));
        append(stbl, box("stsc", stsc));
        append(stbl, box("stsz", stsz));
        append(stbl, box("stco", stco));

        Buf vmhd;
        u32(vmhd, 1);
        zeros(vmhd, 8);
        Buf url;
        u32(url, 1);
        Buf dref;
        u32(dref, 0);
        u32(dref, 1);
        append(dref, box("url ", url));
        Buf minf;
        append(minf, box("vmhd", vmhd));
        append(minf, box("dinf", box("dref", dref)));
        append(minf, box("stbl", stbl));

        Buf mdhd;
        u32(mdhd, 0);
        u32(mdhd, 0);
        u32(mdhd, 0);
        u32(mdhd, kTimescale);
        u32(mdhd, total32);
        u16(mdhd, 0x55c4); // 'und'
        u16(mdhd, 0);
        Buf hdlr;
        u32(hdlr, 0);
        u32(hdlr, 0);
        tag(hdlr, "vide");
        zeros(hdlr, 12);
        hdlr.push_back(0);
        Buf mdia;
        append(mdia, box("mdhd", mdhd));
        append(mdia, box("hdlr", hdlr));
        append(mdia, box("minf", minf));

        Buf tkhd;
        u32(tkhd, 3); // enabled, in movie
        u32(tkhd, 0);
        u32(tkhd, 0);
        u32(tkhd, 1); // track id
        u32(tkhd, 0);
        u32(tkhd, total32);
        zeros(tkhd, 8);
        u32(tkhd, 0);
        u32(tkhd, 0);
        const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
        for (uint32_t m : matrix)
            u32(tkhd, m);
        u32(tkhd, width_ << 16);
        u32(tkhd, height_ << 16);
        Buf trak;
        append(trak, box("tkhd", tkhd));
        append(trak, box("mdia", mdia));

        Buf mvhd;
        u32(mvhd, 0);
        u32(mvhd, 0);
        u32(mvhd, 0);
        u32(mvhd, kTimescale);
        u32(mvhd, total32);
        u32(mvhd, 0x00010000);
        u16(mvhd, 0x0100);
        zeros(mvhd, 10);
        for (uint32_t m : matrix)
            u32(mvhd, m);
        zeros(mvhd, 24);
        u32(mvhd, 2); // next track id
        Buf moov;
        append(moov, box("mvhd", mvhd));
        append(moov, box("trak", trak));
        return box("moov", moov);
    }

    std::FILE *fp_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    uint64_t mdat_pos_ = 0, offset_ = 0;
    Buf sps_, pps_;
    std::vector<Sample> samples_;
    std::vector<std::pair<size_t, size_t>> nals_;
};
