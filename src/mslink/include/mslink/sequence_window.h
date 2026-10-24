#pragma once
#include <cstdint>
namespace mslink {
// Accept reordered packets within 64 slots, reject duplicates and old replays.
// Call only AFTER authentication has succeeded.
struct SequenceWindow {
    uint32_t highest = 0;
    uint64_t seen = 0;
    bool initialized = false;
    // 该 seq 是否已经在窗口内被接受过（重复副本）。用于把"冗余副本"与"真丢包"分开计数。
    // 只读，不改变窗口状态。
    bool duplicate(uint32_t seq) const {
        if (!initialized) return false;
        const int32_t delta = static_cast<int32_t>(seq - highest);
        // seq == highest has already been accepted once; it is a duplicate copy.
        // Newer sequences are not duplicates, and older ones only within the window.
        if (delta > 0) return false;
        const uint32_t back = highest - seq;
        return back < 64 && (seen & (uint64_t(1) << back)) != 0;
    }
    bool accept(uint32_t seq) {
        if (!initialized) { highest=seq;seen=1;initialized=true;return true; }
        const int32_t delta=static_cast<int32_t>(seq-highest);
        if(delta>0){seen=delta>=64?1:(seen<<delta)|1;highest=seq;return true;}
        const uint32_t back=highest-seq;
        if(back>=64 || (seen&(uint64_t(1)<<back)))return false;
        seen|=uint64_t(1)<<back;return true;
    }
};
}
