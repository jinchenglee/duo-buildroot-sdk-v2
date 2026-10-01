#include "../preview_deadline.h"
#include <cassert>
#include <cstdio>

int main()
{
    // A 120 fps camera drives independently capped preview and recording.
    PreviewDeadline preview(15), record(30);
    int previews = 0, recordings = 0;
    for (int frame = 0; frame < 120; ++frame)
    {
        const double ms = frame * (1000.0 / 120);
        previews += preview.due(ms);
        recordings += record.due(ms);
    }
    assert(previews >= 13 && previews <= 15);
    assert(recordings >= 26 && recordings <= 30);
    assert(recordings > previews);
    // A stalled consumer resumes once, without a burst of old deadlines.
    assert(preview.due(5000));
    assert(!preview.due(5000));
    assert(!preview.due(5001));
    assert(preview.due(5067));
    PreviewDeadline uncapped(0);
    assert(uncapped.due(0) && uncapped.due(0) && uncapped.due(1));
    std::puts("Preview cadence, independent recording and stalled-consumer checks passed");
}
