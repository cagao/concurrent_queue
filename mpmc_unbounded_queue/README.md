# multi-producer multi-consumer queue by dvyukov

verify using thread sanitizer:

g++ -g -std=c++11 -fsanitize=thread -fPIE -o mpmc_unbounded_queue mpmc_unbounded_queue.cpp
