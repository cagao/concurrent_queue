# bounded queue by dvyukov

http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

verify using thread sanitizer:

g++ -g -std=c++11 -fsanitize=thread -fPIE -o mpmc_bounded_queue mpmc_bounded_queue.cpp
