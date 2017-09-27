#include <atomic>
#include <array>
#include <thread>
#include <iostream>
#include <xmmintrin.h> // for _mm_pause

#define cache_line_size 64


template <typename T>
class spsc_queue
{
public:
    struct node
    {
        std::atomic<node *> next_;
        T value_;
        node(T value) : value_(value) {}
    };

    spsc_queue()
    {
        node *n = new node(0);
        n->next_ = 0;
        tail_ = head_ = first_ = tail_copy_ = n;
    }

    ~spsc_queue()
    {
        node *n = first_;
        do {
            node *next = n->next_;
            delete n;
            n = next;
        } while (n);
    }

    void enqueue(T v)
    {
        node *n = alloc_node(v);
        n->next_ = nullptr;
        n->value_ = v;

        /*
         * when head_->next_ == tail_->next
         * synchronize with tail_->next
         */
        node *head = head_.load(std::memory_order_relaxed);
        head->next_.store(n, std::memory_order_release);
        head_ = n;
    }

    bool dequeue(T &v)
    {
        /*
         * when head_->next_ == tail_->next
         * synchronize with tail_->next
         */
        node *tail = tail_.load(std::memory_order_relaxed);
        node *tail_next = tail->next_.load(std::memory_order_consume);

        if (tail_next) {
            v = tail_next->value_;
            // synchronize with tail_copy_ load in alloc_node
            tail_.store(tail_next, std::memory_order_release);
            return true;
        }
        return false;
    }
private:

    // producer part
    std::atomic<node *> head_;
    std::atomic<node *> first_; // last unused node
    std::atomic<node *> tail_copy_; // try to catch up tail_ node

    char cache_line_padding_[cache_line_size];

    // consumer part
    std::atomic<node *> tail_; // tail of the queue

    spsc_queue(spsc_queue const&) = delete;
    spsc_queue& operator = (spsc_queue const&) = delete;

public:

    node *alloc_node(T v)
    {
        // first tries to allocate node from internal node cache,
        // if attempt fails, allocates node via ::operator new()

        node *first = first_.load(std::memory_order_relaxed);
        node *tail = tail_copy_.load(std::memory_order_relaxed);
        

        if (first != tail) {
            node *n = first;
            n->value_ = v;
            first_.store(first->next_, std::memory_order_relaxed);
            return n;
        }

        tail = tail_.load(std::memory_order_consume);

        if (first != tail) {
            node *n = first;
            n->value_ = v;
            first_.store(first->next_, std::memory_order_relaxed);
            return n;
        }

        node *n = new node(v);
        return n;
    }
};

static size_t const thread_count = 2;
static size_t const batch_size = 1;
static size_t const iter_count = 2000000;

static std::atomic<bool> volatile g_start{0};

typedef spsc_queue<int> queue_t;

static void thread_func(queue_t &queue, int tid) {
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
        if (tid == 0) {
            for (size_t i = 0; i != batch_size; i += 1) {
                queue.enqueue(i);
            }
        }
        else if (tid == 1) {
            for (size_t i = 0; i != batch_size; i += 1) {
                while (!queue.dequeue(data)) {
                    std::this_thread::yield();
                }
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
            std::bind(thread_func, std::ref(queue), i)
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

