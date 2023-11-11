#ifndef KMEMORY_H
#define KMEMORY_H
#include "kheap.h"

void memcpy(void* dest, const void* src, size_t size);

void memmove(void* dest, const void* src, size_t size);

int memcmp(void* dest, void* src, size_t size);

void memset(void* vaddr, uint8_t val, size_t size);

void zeromem(void* vaddr, size_t size);

void* allocPage();
void* zallocPage();

void* allocPages(size_t pages);
void* zallocPages(size_t pages);

void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t size);

// Placement new operator
inline void* operator new(size_t, void* ptr) noexcept;

// Placement delete operator (optional but recommended for symmetry)
inline void operator delete(void*, void*) noexcept;

// Global new ooperator
void* operator new(size_t size);

// Global delete operator
void operator delete(void* ptr) noexcept;

void operator delete(void* ptr, size_t) noexcept;

#endif
