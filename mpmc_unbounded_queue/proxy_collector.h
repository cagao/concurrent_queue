#ifndef PROXY_COLLECTOR_H
#define PROXY_COLLECTOR_H

#include <cstdint>
#include <cassert>
#include <array>
#include <atomic>
#include <thread>
#include <iostream>

class proxy
{
public:
    typedef int sequence_type;
    struct collector;
    
    struct sequence_collector
    {
        sequence_type sequence_;
        collector* c_;
        
        sequence_collector(collector* c = nullptr, sequence_type sequence = 0) : c_(c), sequence_(sequence) { }
    };
    
    struct collector
    {
        std::atomic<sequence_type> count_;
        std::atomic<sequence_collector> next_;
        std::function<void()> defer_free;
        
        collector(sequence_type count = 0) : count_(count), next_(sequence_collector()), defer_free(nullptr) { }
        void reset()
        {
            count_ = 0;
            next_.store(sequence_collector(), std::memory_order_relaxed);
            defer_free = nullptr;
        }
        ~collector() { }
    };
    
private:
    static const sequence_type GUARD = 1;
    static const sequence_type REFERENCE = 2;
    std::atomic<sequence_collector> tail_; // link other collectors
    std::atomic<sequence_collector> free_head_;
    std::atomic<sequence_collector> free_tail_;
    
    collector* alloc_collector(bool alloc)
    {
        collector *c = nullptr;
        sequence_collector old_free, new_free;
        
        old_free = free_head_.load(std::memory_order_acquire);
        while (old_free.c_ != free_tail_.load(std::memory_order_relaxed).c_) {
            new_free.c_ = old_free.c_->next_.load(std::memory_order_relaxed).c_;
            new_free.sequence_ = old_free.sequence_ + GUARD;
            
            if (free_head_.compare_exchange_strong(old_free, new_free, std::memory_order_acq_rel, std::memory_order_acquire)) {
                c = old_free.c_;
                c->reset();
                break;
            }
        }
               
        if (c == nullptr && alloc) {
            c = new collector();
        }
        
        return c;
    }
    
    void release_adjust(collector* c, sequence_type adjust)
    {
        collector* current;
        collector* next;
        
        sequence_collector free_tail, free_tail_next;
        
        // only GUARD bit cleared can do the deferred free
        sequence_type adjusted_count = REFERENCE - adjust;
        current = c;
        
        // readers using the old tail all release the collector, external + internal = GUARD + REFERENCE
        while ((current->count_.load(std::memory_order_acquire) == adjusted_count)
               // there are still readers using the old tail,
               // clear the GUARD protection and transfer the external to internal
               || current->count_.fetch_sub(adjusted_count, std::memory_order_acq_rel) == adjusted_count) {
            
            next = current->next_.load(std::memory_order_relaxed).c_;
            
            free_tail = free_tail_.load(std::memory_order_consume);
            do {
                free_tail_next = free_tail.c_->next_.load(std::memory_order_relaxed);
            } while (!free_tail_.compare_exchange_weak(free_tail, free_tail_next, std::memory_order_acq_rel, std::memory_order_acquire));
            
            current = next;
            
            // free data queued for deferred deletion (in the next node)
            if (current->defer_free) {
                current->defer_free();
                current->defer_free = nullptr;
            }
            
            adjusted_count = REFERENCE;
        }
    }

    
public:
    proxy()
    {
        collector *c = new collector(GUARD + REFERENCE);
        sequence_collector sc(c, 0);
        
        free_tail_.store(sc, std::memory_order_relaxed);
        tail_.store(sc, std::memory_order_relaxed);
        free_head_.store(sc, std::memory_order_relaxed);
    }
    
    ~proxy()
    {
        collector* current;
        collector* next;
        
        current = free_head_.load(std::memory_order_relaxed).c_;
        while (current) {
            next = current->next_.load(std::memory_order_relaxed).c_;
            delete current;
            current = next;
        }
    }

    collector* acquire()
    {
        sequence_collector old_tail, new_tail;
        
        old_tail = tail_.load(std::memory_order_relaxed);
        do {
            new_tail.sequence_ = old_tail.sequence_ + REFERENCE;
            new_tail.c_ = old_tail.c_;
        } while (!tail_.compare_exchange_weak(old_tail, new_tail, std::memory_order_relaxed, std::memory_order_relaxed));
        
        return old_tail.c_;
    }

    void release(collector* c)
    {
        release_adjust(c, 0);
    }

    template<class F, class... Args>
    void defer_recycle(F&& f, Args&&... args)
    {
        collector *c;
        sequence_collector old_tail, new_tail;
        // link the node to current collector
        
        while ((c = alloc_collector(true)) == nullptr) {
            std::this_thread::yield();
        }
        
        c->count_ = GUARD + 2 * REFERENCE;
        c->defer_free = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        
        /* monkey through the trees queuing trick */
        new_tail.c_ = c;
        new_tail.sequence_ = 0;
        
        old_tail = tail_.load(std::memory_order_consume);
        while (!tail_.compare_exchange_weak(old_tail, new_tail, std::memory_order_acq_rel, std::memory_order_acquire));
        
        old_tail.c_->next_.store(c, std::memory_order_relaxed);
        
        release_adjust(old_tail.c_, (old_tail.sequence_ - GUARD));
    }
};

#endif /* end of PROXY_COLLECTOR_H */
