/*
 * flash_pager.h - Power-loss-safe raw-flash pager for Cardputer ADV.
 *
 * This is an object store, not a malloc replacement. Objects are addressed by
 * namespace/key, updated with copy-on-write pages, and activated atomically by
 * a control-journal record. Reads are copied or exposed only for the lifetime
 * of a visit callback; mapped flash pointers must never escape that callback.
 */

#ifndef FLASH_PAGER_H
#define FLASH_PAGER_H

#include <Arduino.h>
#include <Stream.h>

namespace FlashPager {

constexpr size_t PAGE_SIZE = 4096;
constexpr size_t PAGE_HEADER_SIZE = 64;
constexpr size_t PAGE_PAYLOAD_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;
constexpr uint32_t DEFAULT_OPERATION_TIMEOUT_MS = 10000;

enum class StorageClass : uint8_t {
    Volatile = 0,
    Cache = 1,
    Durable = 2,
};

enum class Status : uint8_t {
    Ok = 0,
    NotInitialized,
    PartitionNotFound,
    FormatRequired,
    FormatFailed,
    Corrupt,
    NoMemory,
    IndexFull,
    ObjectLimit,
    NotFound,
    InvalidArgument,
    Busy,
    WouldBlock,
    NoSpace,
    QuotaExceeded,
    IoError,
    Timeout,
    TransactionFailed,
    CallbackAborted,
};

struct Stats {
    bool initialized = false;
    bool workerTask = false;
    bool cooperativeWriter = false;
    size_t partitionBytes = 0;
    size_t logicalCapacityBytes = 0;
    size_t logicalUsedBytes = 0;
    size_t physicalLiveBytes = 0;
    uint16_t dataSectors = 0;
    uint16_t erasedSectors = 0;
    uint16_t liveSectors = 0;
    uint16_t pendingSectors = 0;
    uint16_t staleSectors = 0;
    uint16_t badSectors = 0;
    uint16_t objectCount = 0;
    uint16_t openObjects = 0;
    uint16_t journalFreeRecords = 0;
    uint8_t queuedWrites = 0;
    uint8_t writeBuffers = 0;
    uint8_t criticalDepth = 0;
};

using VisitCallback = bool (*)(const uint8_t* data, size_t length,
                               size_t logicalOffset, void* context);

// begin() lazily formats an invalid/legacy partition when autoFormat is true.
bool begin(bool autoFormat = true);
bool initialized();
Status lastStatus();
const char* statusText(Status status);
String describe();
Stats stats();
void poll();
bool flushAll(uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);

// Deletes closed objects of a class, oldest-accessed first. targetBytes == 0
// purges all matching objects. Durable data is purged only when explicitly
// requested with StorageClass::Durable.
size_t purge(StorageClass storageClass, size_t targetBytes = 0);

// Prevents the writer/GC from starting a flash program or erase operation.
// Entry waits for an already-running operation to finish. Do not commit or
// write pager data while holding this guard.
bool enterCritical(uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);
void leaveCritical();

class CriticalGuard {
public:
    explicit CriticalGuard(
        uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);
    ~CriticalGuard();
    CriticalGuard(const CriticalGuard&) = delete;
    CriticalGuard& operator=(const CriticalGuard&) = delete;
    bool held() const { return _held; }

private:
    bool _held = false;
};

class Object {
public:
    Object() = default;
    ~Object();
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&& other) noexcept;
    Object& operator=(Object&& other) noexcept;

    bool open(const char* nameSpace, const char* key,
              StorageClass storageClass = StorageClass::Cache,
              size_t quotaBytes = 0);
    void close(bool commitPending = false);
    bool isOpen() const { return _slot >= 0; }

    bool beginTransaction();
    bool inTransaction() const;
    size_t read(size_t offset, void* destination, size_t length);
    size_t write(size_t offset, const void* source, size_t length,
                 bool nonBlocking = false);
    size_t append(const void* source, size_t length,
                  bool nonBlocking = false);
    bool truncate(size_t length);
    bool visit(size_t offset, size_t length, VisitCallback callback,
               void* context);

    // Writes are queued immediately. flush() waits for page writes but does not
    // activate them; commit() appends the atomic control-journal record.
    bool flushAsync();
    bool flush(uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);
    bool commit(uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);
    bool rollback(uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);
    bool erase(uint32_t timeoutMs = DEFAULT_OPERATION_TIMEOUT_MS);

    size_t size() const;
    size_t capacity() const;
    StorageClass storageClass() const;

private:
    int16_t _slot = -1;
    uint32_t _token = 0;
};

// Arduino Stream adapter for network parsers, JSON readers, editors and other
// code that must not depend on a contiguous allocation.
class PagedStream : public Stream {
public:
    PagedStream() = default;
    ~PagedStream() override;
    PagedStream(const PagedStream&) = delete;
    PagedStream& operator=(const PagedStream&) = delete;

    bool open(const char* nameSpace, const char* key,
              StorageClass storageClass = StorageClass::Cache,
              size_t quotaBytes = 0, bool truncateExisting = false,
              bool writeable = true);
    bool close(bool commitPending = true);
    bool seek(size_t position);
    size_t position() const { return _position; }
    size_t size() const { return _object.size(); }
    bool isOpen() const { return _object.isOpen(); }
    Object& object() { return _object; }

    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    using Print::write;

private:
    Object _object;
    size_t _position = 0;
    int _peeked = -1;
    bool _writeable = false;
};

}  // namespace FlashPager

#endif  // FLASH_PAGER_H
