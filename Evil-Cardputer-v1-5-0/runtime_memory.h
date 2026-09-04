/*
 * runtime_memory.h - Bounded runtime allocations for Cardputer ADV.
 *
 * Cardputer ADV has no PSRAM. Callers must preserve enough internal SRAM for
 * WiFi, TLS and display drivers instead of treating every free byte as
 * application storage. Large, non-realtime data can later use a flash-backed
 * store without changing these allocation rules.
 */

#ifndef RUNTIME_MEMORY_H
#define RUNTIME_MEMORY_H

#include <Arduino.h>

namespace RuntimeMemory {

constexpr size_t DEFAULT_INTERNAL_RESERVE = 32U * 1024U;

struct Snapshot {
    size_t freeInternal = 0;
    size_t largestInternal = 0;
    size_t freeExternal = 0;
    size_t largestExternal = 0;
    size_t totalExternal = 0;
};

Snapshot snapshot();
bool externalAvailable();

void* allocateExternal(size_t bytes, bool clear = false);
void* allocateInternal(size_t bytes, bool clear = false,
                       size_t reserve = DEFAULT_INTERNAL_RESERVE);
void* allocatePreferred(size_t bytes, bool clear = false,
                        size_t reserve = DEFAULT_INTERNAL_RESERVE);
void release(void* memory);

String describe();

}  // namespace RuntimeMemory

#endif  // RUNTIME_MEMORY_H
