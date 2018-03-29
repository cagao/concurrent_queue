#ifndef EVENCOUNT_H
#define EVENCOUNT_H

#include <atomic>
#include <semaphore.h>

#ifdef __APPLE__
class eventcount {
public:
    
    eventcount() :  waiting(false) {
        semaphore = sem_open("eventcount", O_CREAT, 0600, 0);
    }
    
    ~eventcount() {
        sem_close(semaphore);
    }
    
    void prepare_wait() {
        waiting.store(true, std::memory_order_seq_cst);
    }
    
    void cancel_wait() {
        waiting.store(false, std::memory_order_release);
    }
    
    void commit_wait() {
        sem_wait(semaphore);
    }
    
    void notify() {
        if (waiting.load(std::memory_order_acquire)) {
            waiting.store(false, std::memory_order_release);
            sem_post(semaphore);
        }
    }
    
    template<typename FUNC>
    auto await(FUNC func) -> decltype(func()) {
        decltype(func()) result = func();
        while (!result) {
            prepare_wait();
            result = func();
            if (result) {
                cancel_wait();
                break;
            }
            commit_wait();
            result = func();
        }
        return result;
    }
    
    
private:
    
    std::atomic<bool> waiting;
    sem_t *semaphore;
    
};
#else
class eventcount {
public:
    
    eventcount() :  waiting(false) {
        sem_init(&semaphore, 0, 0);
    }
    
    ~eventcount() {
        sem_destroy(&semaphore);
    }
    
    void prepare_wait() {
        waiting.store(true, std::memory_order_seq_cst);
    }
    
    void cancel_wait() {
        waiting.store(false, std::memory_order_release);
    }
    
    void commit_wait() {
        sem_wait(&semaphore);
    }
    
    void notify() {
        if (waiting.load(std::memory_order_acquire)) {
            waiting.store(false, std::memory_order_release);
            sem_post(&semaphore);
        }
    }
    
    template<typename FUNC>
    auto out(FUNC func) -> decltype(func()) {
        decltype(func()) result = func();
        while (!result) {
            prepare_wait();
            result = func();
            if (result) {
                cancel_wait();
                break;
            }
            commit_wait();
            result = func();
        }
        return result;
    }
    
    
private:
    
    std::atomic<bool> waiting;
    sem_t semaphore;
    
};
#endif /* end of __APPLE__ */
#endif /* end of EVENTCOUNT_H */
