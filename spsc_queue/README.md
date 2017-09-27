# spsc queue by dvyukov

https://software.intel.com/en-us/articles/single-producer-single-consumer-queue

verify using thread sanitizer:

g++ -g -std=c++11 -fsanitize=thread -fPIE -o spsc_queue spsc_queue.cpp
