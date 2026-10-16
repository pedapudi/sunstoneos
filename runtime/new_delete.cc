#include "platform.h"
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace {
void *Required(void *memory)
{
    if (!memory) {
        platform_puts("FAIL C++ allocation exhausted\n");
        platform_exit(1);
    }
    return memory;
}
void *Aligned(std::size_t size, std::align_val_t alignment) noexcept
{
    return memalign(static_cast<std::size_t>(alignment), size ? size : 1);
}
}

// The runtime has no exception unwinder. Checked forms return allocation failure;
// unchecked forms stop through the secure monitor instead of throwing bad_alloc.
void *operator new(std::size_t size) { return Required(std::malloc(size ? size : 1)); }
void *operator new[](std::size_t size) { return ::operator new(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{ return std::malloc(size ? size : 1); }
void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{ return ::operator new(size, tag); }
void *operator new(std::size_t size, std::align_val_t alignment)
{ return Required(Aligned(size, alignment)); }
void *operator new[](std::size_t size, std::align_val_t alignment)
{ return ::operator new(size, alignment); }
void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{ return Aligned(size, alignment); }
void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &tag) noexcept
{ return ::operator new(size, alignment, tag); }

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete(void *memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void *memory, std::align_val_t, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete[](void *memory, std::align_val_t, const std::nothrow_t &) noexcept { std::free(memory); }
