#include <cstdlib>
#include <cstdio>
#include <vector>
#include <iostream>
#include "mmap_allocator.hpp"

int main() {
    std::vector<int, galaxy::mmap_allocator<int>> vec(16);
    //std::vector<int> vec;
    for (size_t i = 0; i < vec.size(); i++)
        vec[i] = static_cast<int>(i);
    for (size_t i = vec.size(); i-- > 0;)
        std::cout << vec[i] << ' ';
    std::cout << std::endl;
    return 0;
}
