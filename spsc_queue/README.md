# spsc queue by dvyukov

https://software.intel.com/en-us/articles/single-producer-single-consumer-queue

Unbounded single-producer/single-consumer node-based queue. Internal non-reducible cache of nodes is used. Dequeue operation is always wait-free. Enqueue operation is wait-free in common case (when there is available node in the cache), otherwise enqueue operation calls ::operator new(), so probably not wait-free. No atomic RMW operations nor heavy memory fences are used, i.e. enqueue and dequeue operations issue just several plain loads, several plain stores and one conditional branching. Cache-conscious data layout is used, so producer and consumer can work simultaneously causing no cache-coherence traffic.

Single-producer/single-consumer queue can be used for communication with thread which services hardware device (wait-free property is required), or when there are naturally only one producer and one consumer. Also N single-producer/single-consumer queues can be used to construct multi-producer/single-consumer queue, or N^2 queues can be used to construct fully-connected system of N threads (other partially-connected topologies are also possible).

verify using thread sanitizer:

g++ -g -std=c++11 -fsanitize=thread -fPIE -o spsc_queue spsc_queue.cpp
