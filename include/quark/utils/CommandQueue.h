#pragma once

#include "quark/Attributes.h"
#include "quark/third_party/SPSC_Queue/SPSCQueue.h"
#include "quark/utils/InplaceFunction.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

QUARK_BEGIN_NAMESPACE

/// Hands work from one producer thread to a consumer thread, typically an event loop draining it
/// in its step hook: `loop.set_step_hook([&] { queue.drain(); })`. Lock-free single-producer /
/// single-consumer; with several producers, give each its own queue. Commands are stored inline
/// (no allocation): a callable larger than Capacity bytes fails to compile.
template <uint32_t Size = 1024, std::size_t Capacity = 64>
class CommandQueue {
public:
    using Command = InplaceFunction<void(), Capacity>;

    CommandQueue() : queue_{std::make_unique<Queue>()} {}

    /// Producer thread: queues `fn`. False if the queue is full (the consumer is falling behind).
    template <class F>
    bool post(F&& fn) {
        return queue_->tryPush([&](Command* slot) { *slot = Command(std::forward<F>(fn)); });
    }

    /// Consumer thread: runs up to `max` queued commands in order. Returns how many ran.
    std::size_t drain(std::size_t max = static_cast<std::size_t>(-1)) {
        std::size_t n = 0;
        while (n < max && queue_->tryPop([](Command* cmd) {
            (*cmd)();
            *cmd = nullptr;  // release captured state now
        })) {
            ++n;
        }
        return n;
    }

private:
    using Queue = SPSCQueue<Command, Size>;
    std::unique_ptr<Queue> queue_;  // Size * sizeof(Command) bytes: kept off the stack
};

QUARK_END_NAMESPACE
