// meshoptimizer is built without exceptions or the C++ library (-fno-exceptions -fno-rtti
// -nostdinc++); its default allocator calls these, so the DLL needs no libc++.
#include <stddef.h>
#include <stdlib.h>

void* operator new(size_t n) { return malloc(n ? n : 1); }
void* operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
