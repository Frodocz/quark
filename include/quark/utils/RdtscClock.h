#pragma once

#include "quark/Attributes.h"
#include "quark/third_party/tscns/tscns.h"

#include <cstdint>

QUARK_BEGIN_NAMESPACE

class QUARK_API RdtscClock
{
public:
    RdtscClock(const RdtscClock&) = delete;
    RdtscClock& operator=(const RdtscClock&) = delete;

    static int64_t rdtsc() {
        return TSCNS::rdtsc();
    }

    static int64_t tscns() {
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
