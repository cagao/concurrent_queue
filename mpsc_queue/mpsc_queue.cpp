#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <iostream>

#include "eventcount.h"

#include <emmintrin.h>

template<typename T>
class mpsc_queue {
    struct node {
        std::atomic<node*> next_;
        T volatile value_;
        node(T value, node *next = nullptr) : value_(value)
        {
            next_.store(next, std::memory_order_relaxed);
        }
    };

    std::unique_ptr<node> stub_;
    std::atomic<node*> head_;
    std::atomic<node*> tail_;

public:
    mpsc_queue() : stub_(new node(0))
    {
        head_.store(stub_.get(), std::memory_order_relaxed);
        tail_.store(stub_.get(), std::memory_order_relaxed);
    }


    ~mpsc_queue()
    {
        assert(head_.load(std::memory_order_relaxed) ==
                            tail_.load(std::memory_order_relaxed));
    }


public:
    void enqueue(T& value)
    {
        node* n = new node(value);
        node* p = head_.exchange(n, std::memory_order_acq_rel); // serialize producers
        /* if this thread dies, this will be the dangerous zone */
        p->next_.store(n, std::memory_order_seq_cst); // serialize consumer
        // head<-nodeN<-..node1<-tail
    }


    bool dequeue(T& value)
    {
        node* n;
        node* t = tail_.load(std::memory_order_relaxed);
        n = t->next_.load(std::memory_order_acquire); // synchrnize producer
        if (n != nullptr) {
            tail_.store(n, std::memory_order_relaxed);
            value = n->value_;
            free(t);
            return true;
        }
        return false;
    }
};


#define PRODUCERS 4
#define CONSUMERS 1
#define THREADS (PRODUCERS + CONSUMERS)
#define ITERS 600000

eventcount ec;
mpsc_queue<int> queue;
std::atomic<int> count;
static std::atomic<bool> volatile g_start{0};

static void thread_func(unsigned tidx) {
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

    int i;
    if (tidx < PRODUCERS) {
        for (i = 0 + tidx * ITERS; i < ITERS + tidx * ITERS; ++i) {
            queue.enqueue(i);
            ec.notify();
        }
    } else {
        do {
            ec.await([&]{
                if (queue.dequeue(i)) {
                    return true;
                } else {
                    return false;
                }
            });
        } while (count.fetch_add(1, std::memory_order_relaxed) != (PRODUCERS * ITERS - 1));
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

int main()
{

    std::array<std::thread, THREADS> threads;
    for (size_t i = 0; i != THREADS; ++i) {
        threads[i] = std::move(std::thread(
            std::bind(thread_func, i)));
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    g_start = 1;
    uint64_t start = rdtsc();

    for (size_t i = 0; i != THREADS; ++i) {
        threads[i].join();
    }

    uint64_t end = rdtsc();
    uint64_t time = end - start;
    std::cout << "cycles/op="
        << time / (ITERS * THREADS)
        << std::endl;

    return 0;
}
