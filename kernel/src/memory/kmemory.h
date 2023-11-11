#ifndef KMEMORY_H
#define KMEMORY_H
#include "kheap.h"

void memcpy(void* dest, const void* src, size_t size);

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

#endif
