/* Multi-producer/multi-consumer bounded queue
 * Copyright (c) 2010-2011 Dmitry Vyukov. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY DMITRY VYUKOV "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL DMITRY VYUKOV OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of Dmitry Vyukov.
 */

#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <array>
#include <xmmintrin.h> // for _mm_pause

template<typename T, size_t buffer_size>
class mpmc_bounded_queue
{
private:
    struct cell_t {
        std::atomic<size_t> sequence_;
        T                   data_;
    };

    static size_t const     cacheline_size = 64;
    typedef char            cacheline_pad_t [cacheline_size];

    cacheline_pad_t         pad0_;
    cell_t *const           buffer_;
    size_t const            buffer_mask_ = buffer_size-1;
    cacheline_pad_t         pad1_;
    std::atomic<size_t>     enqueue_pos_;
    cacheline_pad_t         pad2_;
    std::atomic<size_t>     dequeue_pos_;
    cacheline_pad_t         pad3_;

public:
    static_assert(
            (buffer_size >= 2) && ((buffer_size & (buffer_size - 1)) == 0),
            "bad buffer size, no room for mask");
    mpmc_bounded_queue(mpmc_bounded_queue const&) = delete;
    void operator = (mpmc_bounded_queue const&) = delete;

public:
    mpmc_bounded_queue()
        : buffer_(new cell_t[buffer_size])
    {
        for (size_t i = 0; i != buffer_size; i += 1) {
            buffer_[i].sequence_.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    ~mpmc_bounded_queue() {
        delete[] buffer_;
    }

    bool enqueue(T const& data) {
        cell_t* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & buffer_mask_];
            size_t seq = cell->sequence_.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                            pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data_ = data;
        cell->sequence_.store(pos + 1, std::memory_order_release);

        return true;
    }

    bool dequeue(T& data) {
        cell_t* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & buffer_mask_];
            size_t seq = cell->sequence_.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                            pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        data = cell->data_;
        cell->sequence_.store(pos + buffer_mask_ + 1, std::memory_order_release);

        return true;
    }
};



static size_t const thread_count = 4;
static size_t const batch_size = 1;
static size_t const iter_count = 2000000;

static std::atomic<bool> volatile g_start{0};

typedef mpmc_bounded_queue<int, 1024> queue_t;

static void thread_func(queue_t &queue) {
    int data;

    std::hash<std::thread::id> hasher;
    std::srand((unsigned)time(0) + (unsigned)hasher(std::this_thread::get_id()));
    size_t pause = std::rand() % 1000;

    while (g_start == 0) {
        std::this_thread::yield();
    }

    for (size_t i = 0; i != pause; i += 1) {
        _mm_pause();
    }

    for (size_t iter = 0; iter != iter_count; ++iter) {
        for (size_t i = 0; i != batch_size; i += 1) {
            while (!queue.enqueue(i)) {
                std::this_thread::yield();
            }
        }
        for (size_t i = 0; i != batch_size; i += 1) {
            while (!queue.dequeue(data)) {
                std::this_thread::yield();
            }
        }
    }
}

static inline uint64_t rdtsc() {
    uint64_t lo, hi;
    __asm__ volatile ("rdtsc"
            : "=a" (lo), "=d"(hi) /*outputs */
            : /* no input parameters */
            : "%ebx", "%ecx", "memory"); /* clobbers */
    return lo | (hi << 32);
}

int main() {
    queue_t queue;

    std::array<std::thread, thread_count> threads;
    for (size_t i = 0; i != thread_count; ++i) {
        threads[i] = std::move(std::thread(
            std::bind(thread_func, std::ref(queue))
            ));
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    uint64_t start = rdtsc();
    g_start = 1;

    for (size_t i = 0; i != thread_count; ++i) {
        threads[i].join();
    }

    uint64_t end = rdtsc();
    uint64_t time = end - start;
    std::cout << "cycles/op="
        << time / (batch_size * iter_count * 2 * thread_count)
        << std::endl;
}
