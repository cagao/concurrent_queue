/*
 * Word-Based Portable Proxy Garbage Collector
 * Copyright (C) 2010 Christopher Michael Thomasson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <cassert>
#include <cstdio>
#include <cstddef>
#include <array>
#include <atomic>
#include <thread>
#include <iostream>

struct node
{
    std::atomic<node*> next_;
    node* defer_next_;
};

template<size_t T_defer_limit, size_t T_collector_size = 4>
class proxy
{
    static void prv_destroy(node* n)
    {
        while (n)
        {
            node* next = n->defer_next_;
            delete n;
            n = next;
        }
    }

public:
    class collector
    {
    friend class proxy;
   
   
    private:
        std::atomic<node*> defer_;
        std::atomic<unsigned int> defer_count_;
        std::atomic<unsigned int> count_;
   
    public:
        collector() : defer_(nullptr), defer_count_(0), count_(0) {}
   
        ~collector()
        {
            prv_destroy(defer_.load(std::memory_order_relaxed));
        }
    };


private:
    // index for collector array
    std::atomic<unsigned int> current_;
    std::atomic<bool> quiesce_;
    node* defer_;
    collector collectors_[T_collector_size];
    unsigned collector_size_mask_;
    static_assert(T_collector_size >= 2 && T_collector_size <= 16, 
        "number of collectors must be between 2 and 16");

public:
    proxy() : current_(0), quiesce_(false), defer_(nullptr),
                collector_size_mask_(T_collector_size - 1) {}

    ~proxy()
    {
        prv_destroy(defer_);
    }

private:
    void prv_quiesce_begin()
    {
        // Try to begin the quiescence process.
        if (! quiesce_.exchange(true, std::memory_order_acquire))
        {
            // advance the current collector and grab the old one.
            unsigned int old = current_.load(std::memory_order_relaxed) & 0xFU;
            old = current_.exchange((old + 1) & collector_size_mask_, std::memory_order_acq_rel);
            collector& c = collectors_[old & 0xFU];
            
            // decode reference count.
            unsigned int refs = old & 0xFFFFFFF0U;
            
            // verify reference count and previous collector index.
            assert(! (refs & 0x10U) && (old & 0xFU) == (&c - collectors_));
            
            // increment and generate an odd reference count.
            if (c.count_.fetch_add(refs + 0x10U, std::memory_order_release) == -refs)
            {
                // odd reference count and drop-to-zero condition detected!
                prv_quiesce_complete(c);
            }
        }
    }


    void prv_quiesce_complete(collector& c)
    {
        // the collector `c' is now in a quiescent state! :^)
        std::atomic_thread_fence(std::memory_order_acquire);
        
        // maintain the back link and obtain "fresh" objects from
        // this collection.
        node* n = defer_;
        defer_ = c.defer_.load(std::memory_order_relaxed);
        c.defer_.store(0, std::memory_order_relaxed);
        
        // verify and reset the reference count.
        assert(c.count_.load(std::memory_order_relaxed) == 0x10U);
        c.count_.store(0, std::memory_order_relaxed);
        c.defer_count_.store(0, std::memory_order_relaxed);
        
        // release the quiesce lock.
        quiesce_.store(false, std::memory_order_release);
        
        // destroy nodes.
        prv_destroy(n);
    }

public:
    collector& acquire()
    {
        // increment the master count _and_ obtain current collector.
        unsigned int current =
        current_.fetch_add(0x20U, std::memory_order_acquire);
    
        // decode the collector index.
        return collectors_[current & 0xFU];
    }


    void release(collector& c)
    {
        // decrement the collector.
        unsigned int count =
        c.count_.fetch_sub(0x20U, std::memory_order_release);
    
        // check for the completion of the quiescence process.
        if ((count & 0xFFFFFFF0U) == 0x30U) {
            // odd reference count and drop-to-zero condition detected!
            prv_quiesce_complete(c);
        }
    }


    collector& sync(collector& c)
    {
        // check if the `c' is in the middle of a quiescence process.
        if (c.count_.load(std::memory_order_relaxed) & 0x10U) {
            // drop `c' and get the next collector.
            release(c);
    
            return acquire();
        }
    
        return c;
    }


    void collect()
    {
        prv_quiesce_begin();
    }


    void collect(collector& c, node* n)
    {
        if (! n) return;
        
        // link node into the defer list.
        node* prev = c.defer_.exchange(n, std::memory_order_relaxed);
        n->defer_next_ = prev;
        
        // bump the defer count and begin quiescence process if over
        // the limit.
        unsigned int count =
        c.defer_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        
        if (count >= (T_defer_limit / 2))
        {
            prv_quiesce_begin();
        }
    }
};




// you're basic lock-free stack...
// well, minus ABA counter and DWCAS of course! ;^)
class stack
{
    std::atomic<node*> head_;

public:
    stack() : head_(nullptr) {}

    void push(node* n)
    {
        node* head = head_.load(std::memory_order_relaxed);
        do
        {
            n->next_.store(head, std::memory_order_relaxed);
        } while (! head_.compare_exchange_weak(head, n, std::memory_order_release));
    }
    
    
    node* flush()
    {
        return head_.exchange(NULL, std::memory_order_acquire);
    }
    
    
    node* get_head()
    {
        return head_.load(std::memory_order_acquire);
    }
    
    node* pop()
    {
        node* head = head_.load(std::memory_order_acquire);
        node* xchg;
        
        do
        {
            if (! head) return nullptr;
            xchg = head->next_.load(std::memory_order_relaxed);
        } while (! head_.compare_exchange_weak(head, xchg, std::memory_order_acquire));
        
        return head;
    }
};

#define ITERS 150000
#define DEFER 6
#define WRITERS 3
#define READERS 5
#define REAPERS 2
#define THREADS (WRITERS + READERS + REAPERS)


typedef proxy<DEFER, 4> proxy_type;

proxy_type g_proxy;
stack g_stack;
std::atomic<unsigned> g_writers{WRITERS}; // for proper test termination only.

void thread_func(size_t tidx)
{
    if (tidx < READERS) {
        proxy_type::collector* c = &g_proxy.acquire();
   
        // readers.
        while (g_writers)
        {
   
            node* n = g_stack.get_head();
   
            while (n)
            {
                node* next = n->next_.load(std::memory_order_relaxed);
                n = next;
            }
   
            c = &g_proxy.sync(*c);
   
            std::this_thread::yield();
        }
        g_proxy.release(*c);
   
    } else if (tidx < WRITERS + READERS) {
        // writers.
        unsigned count = 0;
   
        for (unsigned int i = 0; i < ITERS; ++i) {
            g_stack.push(new node);
       
            if (! (i % 2)) {
                proxy_type::collector& c = g_proxy.acquire();
                g_proxy.collect(c, g_stack.pop());
                g_proxy.release(c);
                std::this_thread::yield();
            }
        }
       
        for (unsigned int i = count; i < ITERS; ++i)
        {
            proxy_type::collector& c = g_proxy.acquire();
            g_proxy.collect(c, g_stack.pop());
            g_proxy.release(c);
        }
        --g_writers;
       
    } else if (tidx < WRITERS + READERS + REAPERS) {
        // reapers.
        while (g_writers)
        {
            g_proxy.collect();
            std::this_thread::yield();
        }
    }
}

int main()
{
    std::array<std::thread, THREADS> threads;
    for (size_t i = 0; i < THREADS; ++i) {
        threads[i] = std::move(std::thread(thread_func, i));
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    for (size_t i = 0; i < THREADS; ++i) {
        threads[i].join();
    }

    return 0;
}
