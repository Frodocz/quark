#pragma once

#include "quark/Attributes.h"
#include "quark/third_party/tscns/tscns.h"

#include <cstdint>

QUARK_BEGIN_NAMESPACE

// rdtsc(): raw TSC ticks. tscns(): nanoseconds since the epoch from the TSC, ~10x cheaper than
// the system clock; the first call blocks ~20 ms to calibrate. sysns(): the system clock itself
// (std::chrono::system_clock), the source of truth for wall time.
//
// tscns() tracks the system clock only if calibrate() runs at least every few seconds, from a
// single thread. quark's log backend does it (quill's backend thread, or the thread running
// LogPoller) once init_log() has run; a process without quark logging must call calibrate()
// itself, e.g. once a second from a housekeeping thread, and never from two threads.
class QUARK_API RdtscClock
{
public:
    RdtscClock(const RdtscClock&) = delete;
    RdtscClock& operator=(const RdtscClock&) = delete;

    static QUARK_ALWAYS_INLINE int64_t rdtsc() {
        return TSCNS::rdtsc();
    }

    static QUARK_ALWAYS_INLINE int64_t tscns() {
        return instance().tsc_clock_.rdns();
    }

    static int64_t sysns() {
        return TSCNS::rdsysns();
    }

    static void calibrate() {
        instance().tsc_clock_.calibrate();
    }
private:
    TSCNS tsc_clock_;

    RdtscClock() {
        tsc_clock_.init();
    }

    static RdtscClock& instance() {
        static RdtscClock inst;
        return inst;
    }
};

QUARK_END_NAMESPACE
