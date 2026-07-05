#ifndef __PCH_HPP__
#define __PCH_HPP__

#include <boost/asio/steady_timer.hpp>

#pragma GCC diagnostic push 
#pragma GCC diagnostic ignored "-Wparentheses"
#include <boost/lockfree/spsc_queue.hpp>
#pragma GCC diagnostic pop

#include <boost/chrono.hpp>

#include <algorithm>
#include <stdint.h>
#include <cassert>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdio.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <SDL2/SDL.h>

#include <printf.h>

#endif // __PCH_HPP__