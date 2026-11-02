// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// A lock-free single-producer / single-consumer ring of fixed-size events
// (plan 869ejxng3 + 1amfwm7e6cw rev 3, section 4.3e). The audio thread pushes
// and never blocks or allocates; the main loop drains it every 10 ms and
// writes the journal and the stdout ack lines. The OSC thread latches its
// events into a ring of its own (the latching rule of section 4.3h, h.1).
#ifndef RTLOG_H
#define RTLOG_H

#include <atomic>
#include <cstddef>

template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    // Producer. False (and the event is counted as dropped) when full.
    bool push(const T& v)
    {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= N) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    // Consumer.
    bool pop(T& v)
    {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;
        v = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    size_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    T buf_[N];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::atomic<size_t> dropped_{0};
};

#endif  // RTLOG_H
