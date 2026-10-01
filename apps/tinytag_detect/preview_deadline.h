#pragma once

// Caller supplies monotonic milliseconds. Late frames never cause catch-up
// submissions: each accepted frame starts a new minimum spacing interval.
class PreviewDeadline
{
public:
    explicit PreviewDeadline(unsigned fps) : period_ms_(fps ? 1000.0 / fps : 0.0) {}
    bool due(double now_ms)
    {
        if (period_ms_ && now_ms < next_ms_)
            return false;
        next_ms_ = now_ms + period_ms_;
        return true;
    }
private:
    double period_ms_;
    double next_ms_ = 0.0;
};
