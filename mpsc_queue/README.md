# multi-producer single-consumer queue by dvyukov

verify using thread sanitizer:

g++ -g -std=c++11 -fsanitize=thread -fPIE -o mpsc_queue mpsc_queue.cpp
