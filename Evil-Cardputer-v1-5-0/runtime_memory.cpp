/*
 * runtime_memory.cpp - Capability-aware, reserve-preserving allocator.
 */

#include "runtime_memory.h"

#include <esp_heap_caps.h>

namespace RuntimeMemory {
namespace {

constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t EXTERNAL_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void* allocateCaps(size_t bytes, bool clear, uint32_t caps) {
    if (bytes == 0) return nullptr;
    return clear ? heap_caps_calloc(1, bytes, caps)
                 : heap_caps_malloc(bytes, caps);
}

}  // namespace

Snapshot snapshot() {
    Snapshot result;
    result.freeInternal = heap_caps_get_free_size(INTERNAL_CAPS);
    result.largestInternal = heap_caps_get_largest_free_block(INTERNAL_CAPS);
    result.freeExternal = heap_caps_get_free_size(EXTERNAL_CAPS);
    result.largestExternal = heap_caps_get_largest_free_block(EXTERNAL_CAPS);
    result.totalExternal = heap_caps_get_total_size(EXTERNAL_CAPS);
    return result;
}

bool externalAvailable() {
    const Snapshot current = snapshot();
    return current.totalExternal > 0 && current.largestExternal > 0;
}

void* allocateExternal(size_t bytes, bool clear) {
    if (bytes == 0) return nullptr;
    const Snapshot current = snapshot();
    if (current.totalExternal == 0 || current.largestExternal < bytes ||
        current.freeExternal < bytes) {
        return nullptr;
    }
    return allocateCaps(bytes, clear, EXTERNAL_CAPS);
}

void* allocateInternal(size_t bytes, bool clear, size_t reserve) {
    if (bytes == 0) return nullptr;
    const Snapshot current = snapshot();
    if (current.largestInternal < bytes || current.freeInternal <= reserve ||
        bytes > current.freeInternal - reserve) {
        return nullptr;
    }
    return allocateCaps(bytes, clear, INTERNAL_CAPS);
}

void* allocatePreferred(size_t bytes, bool clear, size_t reserve) {
    void* memory = allocateExternal(bytes, clear);
    if (!memory) memory = allocateInternal(bytes, clear, reserve);
    return memory;
}

void release(void* memory) {
    if (memory) heap_caps_free(memory);
}

String describe() {
    const Snapshot current = snapshot();
    String result;
    result.reserve(96);
    result += "internal free=";
    result += current.freeInternal;
    result += " largest=";
    result += current.largestInternal;
    result += " external=";
    result += current.freeExternal;
    return result;
}

}  // namespace RuntimeMemory
