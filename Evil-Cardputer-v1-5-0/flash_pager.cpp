/*
 * flash_pager.cpp - Log-structured raw-flash pager for Cardputer ADV.
 */

#include "flash_pager.h"

#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stddef.h>
#include <string.h>
#include <utility>

#include "runtime_memory.h"

namespace FlashPager {
namespace {

constexpr char PARTITION_LABEL[] = "swap";
constexpr esp_partition_subtype_t PARTITION_SUBTYPE =
    static_cast<esp_partition_subtype_t>(0x40);
constexpr uint32_t FORMAT_VERSION = 1;
constexpr uint32_t SUPER_MAGIC = 0x50414752U;    // PAGR
constexpr uint32_t CONTROL_MAGIC = 0x50474354U;  // PGCT
constexpr uint32_t DATA_MAGIC = 0x50474454U;     // PGDT
constexpr uint32_t COMMIT_WORD = 0xC04D17EDU;
constexpr uint8_t RECORD_VERSION = 1;
constexpr uint8_t CONTROL_OBJECT_STATE = 1;
constexpr uint8_t DATA_OBJECT_PAGE = 1;
constexpr uint16_t CONTROL_FLAG_DELETED = 0x0100;
constexpr uint16_t CONTROL_CLASS_MASK = 0x0003;

constexpr size_t SUPERBLOCK_SECTORS = 2;
constexpr size_t JOURNAL_SECTORS = 8;
constexpr size_t DATA_FIRST_SECTOR = SUPERBLOCK_SECTORS + JOURNAL_SECTORS;
constexpr size_t JOURNAL_RECORDS_PER_SECTOR = PAGE_SIZE / PAGE_HEADER_SIZE;
constexpr size_t MAX_PARTITION_BYTES = 0x2E0000;
constexpr size_t MAX_DATA_SECTORS =
    MAX_PARTITION_BYTES / PAGE_SIZE - DATA_FIRST_SECTOR;
constexpr size_t INDEX_BUCKETS = 1024;
constexpr size_t MAX_OBJECTS = 32;
constexpr size_t WRITE_JOB_LIMIT = 2;
constexpr size_t MMAP_WINDOW_BYTES = 64U * 1024U;
constexpr size_t ERASED_TARGET = 48;
constexpr size_t ERASED_LOW_WATERMARK = 24;
constexpr size_t ERASED_EMERGENCY = 8;
constexpr size_t PAGER_INTERNAL_RESERVE = 38U * 1024U;
constexpr uint16_t INDEX_EMPTY = 0;
constexpr uint16_t INDEX_TOMBSTONE = 0xFFFF;
constexpr uint32_t WORKER_STACK_BYTES = 4096;
constexpr UBaseType_t WORKER_PRIORITY = 1;
constexpr BaseType_t WORKER_CORE = 1;

#pragma pack(push, 1)
struct Superblock {
    uint32_t magic;
    uint16_t version;
    uint16_t headerBytes;
    uint32_t sectorBytes;
    uint32_t payloadBytes;
    uint32_t partitionBytes;
    uint32_t dataFirstSector;
    uint32_t dataSectorCount;
    uint32_t formatGeneration;
    uint8_t sipKey[16];
    uint64_t uuid;
    uint32_t crc32;
    uint32_t commitWord;
};

struct ControlRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t recordType;
    uint16_t flags;
    uint64_t storeHash;
    uint32_t keyTag;
    uint32_t activeEpoch;
    uint32_t logicalSize;
    uint32_t quota;
    uint32_t sequence;
    uint32_t transactionId;
    uint8_t reserved[16];
    uint32_t crc32;
    uint32_t commitWord;
};

struct DataHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t recordType;
    uint16_t flags;
    uint64_t storeHash;
    uint32_t objectEpoch;
    uint16_t logicalPage;
    uint16_t payloadLength;
    uint32_t generation;
    uint32_t logicalSize;
    uint32_t transactionId;
    uint32_t payloadCrc32;
    uint32_t headerCrc32;
    uint32_t generationInverse;
    uint32_t keyTag;
    uint8_t reserved[8];
    uint32_t commitWord;
};
#pragma pack(pop)

static_assert(sizeof(Superblock) == PAGE_HEADER_SIZE,
              "Pager superblock must be 64 bytes");
static_assert(sizeof(ControlRecord) == PAGE_HEADER_SIZE,
              "Pager control record must be 64 bytes");
static_assert(sizeof(DataHeader) == PAGE_HEADER_SIZE,
              "Pager data header must be 64 bytes");
static_assert(MAX_DATA_SECTORS < INDEX_TOMBSTONE - 1,
              "Physical sector id must fit the compact index");

enum class SectorState : uint8_t {
    Unknown,
    Erased,
    Reserved,
    Writing,
    Pending,
    Live,
    Stale,
    Erasing,
    Bad,
};

enum class JobState : uint8_t {
    Free,
    Filling,
    Queued,
    Writing,
    Done,
    Failed,
};

enum class CommandKind : uint8_t { None, AppendControl, EraseData };
enum class CommandState : uint8_t { Idle, Pending, Running, Done };

struct ObjectState {
    bool used = false;
    bool reserved = false;
    bool deleted = false;
    uint8_t openCount = 0;
    uint16_t flags = 0;
    uint64_t storeHash = 0;
    uint32_t keyTag = 0;
    uint32_t activeEpoch = 0;
    uint32_t logicalSize = 0;
    uint32_t quota = 0;
    uint32_t sequence = 0;
    uint32_t transactionId = 0;
    uint32_t lastAccessMs = 0;
};

struct TransactionState {
    bool active = false;
    bool failed = false;
    int16_t objectSlot = -1;
    uint32_t ownerToken = 0;
    uint32_t candidateEpoch = 0;
    uint32_t transactionId = 0;
    size_t logicalSize = 0;
    Status failure = Status::Ok;
    uint16_t* pendingByPage = nullptr;
};

struct WriteJob {
    JobState state = JobState::Free;
    uint8_t* image = nullptr;
    uint32_t queueSequence = 0;
    uint32_t ownerToken = 0;
    uint32_t transactionId = 0;
    uint16_t targetSector = 0;
    uint16_t logicalPage = 0;
    int16_t objectSlot = -1;
    Status result = Status::Ok;
};

struct WorkerCommand {
    CommandKind kind = CommandKind::None;
    CommandState state = CommandState::Idle;
    ControlRecord record = {};
    uint16_t dataSector = 0;
    Status result = Status::Ok;
};

struct MappedWindow {
    const uint8_t* pointer = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    size_t offset = 0;
    size_t length = 0;
};

const esp_partition_t* g_partition = nullptr;
Superblock g_superblock = {};
ObjectState g_objects[MAX_OBJECTS];
uint8_t* g_metadata = nullptr;
SectorState* g_sectorState = nullptr;
uint8_t* g_sectorPins = nullptr;
uint16_t* g_index = nullptr;
uint16_t* g_pendingByPage = nullptr;
uint16_t g_indexTombstones = 0;
TransactionState g_transaction;
WriteJob g_jobs[WRITE_JOB_LIMIT];
WorkerCommand g_command;
MappedWindow g_window;

SemaphoreHandle_t g_stateMutex = nullptr;
SemaphoreHandle_t g_flashMutex = nullptr;
TaskHandle_t g_workerTask = nullptr;
bool g_initialized = false;
bool g_beginning = false;
bool g_cooperativeWriter = false;
volatile bool g_flashBusy = false;
uint8_t g_criticalDepth = 0;
uint8_t g_jobCount = 0;
uint16_t g_dataSectorCount = 0;
uint16_t g_allocCursor = 0;
uint16_t g_gcCursor = 0;
uint32_t g_dataGeneration = 0;
uint32_t g_controlSequence = 0;
uint32_t g_transactionSequence = 0;
uint32_t g_jobSequence = 0;
uint32_t g_nextHandleToken = 1;
Status g_lastStatus = Status::NotInitialized;
uint8_t g_journalNext[JOURNAL_SECTORS] = {};
bool g_journalErased[JOURNAL_SECTORS] = {};
uint8_t g_journalActive = 0xFF;

constexpr uint32_t CRC_NIBBLE_TABLE[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU};

void setStatus(Status status) { g_lastStatus = status; }

bool takeState(TickType_t ticks = portMAX_DELAY) {
    return g_stateMutex && xSemaphoreTake(g_stateMutex, ticks) == pdTRUE;
}

void giveState() {
    if (g_stateMutex) xSemaphoreGive(g_stateMutex);
}

bool takeFlash(TickType_t ticks = portMAX_DELAY) {
    return g_flashMutex && xSemaphoreTake(g_flashMutex, ticks) == pdTRUE;
}

void giveFlash() {
    if (g_flashMutex) xSemaphoreGive(g_flashMutex);
}

bool newer(uint32_t candidate, uint32_t current) {
    return static_cast<int32_t>(candidate - current) > 0;
}

uint32_t crcUpdate(uint32_t crc, const void* source, size_t length) {
    const uint8_t* data = static_cast<const uint8_t*>(source);
    while (length--) {
        crc ^= *data++;
        crc = (crc >> 4U) ^ CRC_NIBBLE_TABLE[crc & 0x0FU];
        crc = (crc >> 4U) ^ CRC_NIBBLE_TABLE[crc & 0x0FU];
    }
    return crc;
}

uint32_t crc32(const void* source, size_t length) {
    return crcUpdate(0xFFFFFFFFU, source, length) ^ 0xFFFFFFFFU;
}

uint32_t superCrc(const Superblock& input) {
    Superblock copy = input;
    copy.crc32 = 0;
    copy.commitWord = 0xFFFFFFFFU;
    return crc32(&copy, offsetof(Superblock, commitWord));
}

uint32_t controlCrc(const ControlRecord& input) {
    ControlRecord copy = input;
    copy.crc32 = 0;
    copy.commitWord = 0xFFFFFFFFU;
    return crc32(&copy, offsetof(ControlRecord, commitWord));
}

uint32_t headerCrc(const DataHeader& input) {
    DataHeader copy = input;
    copy.headerCrc32 = 0;
    copy.commitWord = 0xFFFFFFFFU;
    return crc32(&copy, offsetof(DataHeader, commitWord));
}

bool allErased(const void* source, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0xFFU) return false;
    }
    return true;
}

uint64_t rotateLeft(uint64_t value, uint8_t count) {
    return (value << count) | (value >> (64U - count));
}

uint64_t loadLe64(const uint8_t* data) {
    uint64_t value = 0;
    for (uint8_t i = 0; i < 8; ++i) value |= uint64_t(data[i]) << (8U * i);
    return value;
}

void sipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1;
    v1 = rotateLeft(v1, 13);
    v1 ^= v0;
    v0 = rotateLeft(v0, 32);
    v2 += v3;
    v3 = rotateLeft(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotateLeft(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotateLeft(v1, 17);
    v1 ^= v2;
    v2 = rotateLeft(v2, 32);
}

class SipHasher {
public:
    explicit SipHasher(const uint8_t key[16]) {
        const uint64_t k0 = loadLe64(key);
        const uint64_t k1 = loadLe64(key + 8);
        _v0 = 0x736F6D6570736575ULL ^ k0;
        _v1 = 0x646F72616E646F6DULL ^ k1;
        _v2 = 0x6C7967656E657261ULL ^ k0;
        _v3 = 0x7465646279746573ULL ^ k1;
    }

    void update(const void* source, size_t length) {
        const uint8_t* data = static_cast<const uint8_t*>(source);
        _length += length;
        while (length) {
            _tail[_tailLength++] = *data++;
            --length;
            if (_tailLength == sizeof(_tail)) {
                compress(loadLe64(_tail));
                _tailLength = 0;
            }
        }
    }

    uint64_t finish() {
        uint64_t last = uint64_t(_length & 0xFFU) << 56U;
        for (uint8_t i = 0; i < _tailLength; ++i)
            last |= uint64_t(_tail[i]) << (8U * i);
        compress(last);
        _v2 ^= 0xFFU;
        for (uint8_t i = 0; i < 4; ++i) sipRound(_v0, _v1, _v2, _v3);
        return _v0 ^ _v1 ^ _v2 ^ _v3;
    }

private:
    void compress(uint64_t word) {
        _v3 ^= word;
        sipRound(_v0, _v1, _v2, _v3);
        sipRound(_v0, _v1, _v2, _v3);
        _v0 ^= word;
    }

    uint64_t _v0 = 0;
    uint64_t _v1 = 0;
    uint64_t _v2 = 0;
    uint64_t _v3 = 0;
    size_t _length = 0;
    uint8_t _tail[8] = {};
    uint8_t _tailLength = 0;
};

void calculateKey(const char* nameSpace, const char* key, uint64_t& hash,
                  uint32_t& tag) {
    static const uint8_t separator = 0x1FU;
    SipHasher sip(g_superblock.sipKey);
    uint32_t crc = 0xFFFFFFFFU;
    const char* ns = nameSpace ? nameSpace : "";
    const char* objectKey = key ? key : "";
    sip.update(ns, strlen(ns));
    sip.update(&separator, 1);
    sip.update(objectKey, strlen(objectKey));
    crc = crcUpdate(crc, ns, strlen(ns));
    crc = crcUpdate(crc, &separator, 1);
    crc = crcUpdate(crc, objectKey, strlen(objectKey));
    hash = sip.finish();
    tag = crc ^ 0xFFFFFFFFU;
}

uint64_t mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

size_t dataOffset(uint16_t dataSector) {
    return (DATA_FIRST_SECTOR + dataSector) * PAGE_SIZE;
}

size_t journalOffset(uint8_t sector, uint8_t slot) {
    return (SUPERBLOCK_SECTORS + sector) * PAGE_SIZE +
           size_t(slot) * PAGE_HEADER_SIZE;
}

void unmapWindowLocked() {
    if (g_window.pointer) {
        esp_partition_munmap(g_window.handle);
        g_window = {};
    }
}

bool mapForOffsetLocked(size_t offset) {
    const size_t windowOffset = offset & ~(MMAP_WINDOW_BYTES - 1U);
    if (g_window.pointer && g_window.offset == windowOffset) return true;
    unmapWindowLocked();
    if (!g_partition || windowOffset >= g_partition->size) return false;
    size_t length = g_partition->size - windowOffset;
    if (length > MMAP_WINDOW_BYTES) length = MMAP_WINDOW_BYTES;
    const void* pointer = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(g_partition, windowOffset, length,
                           ESP_PARTITION_MMAP_DATA, &pointer,
                           &handle) != ESP_OK) {
        return false;
    }
    g_window.pointer = static_cast<const uint8_t*>(pointer);
    g_window.handle = handle;
    g_window.offset = windowOffset;
    g_window.length = length;
    return true;
}

bool flashRead(size_t offset, void* destination, size_t length) {
    if (!destination || !g_partition || offset > g_partition->size ||
        length > g_partition->size - offset || !takeFlash()) {
        return false;
    }
    uint8_t* output = static_cast<uint8_t*>(destination);
    bool ok = true;
    while (length) {
        if (mapForOffsetLocked(offset)) {
            const size_t inside = offset - g_window.offset;
            size_t amount = g_window.length - inside;
            if (amount > length) amount = length;
            memcpy(output, g_window.pointer + inside, amount);
            output += amount;
            offset += amount;
            length -= amount;
        } else {
            ok = esp_partition_read(g_partition, offset, output, length) ==
                 ESP_OK;
            length = 0;
        }
    }
    giveFlash();
    return ok;
}

bool flashVisit(size_t flashOffset, size_t length, size_t logicalOffset,
                VisitCallback callback, void* context) {
    if (!callback || !g_partition || !takeFlash()) return false;
    bool ok = true;
    uint8_t fallback[256];
    while (length && ok) {
        if (mapForOffsetLocked(flashOffset)) {
            const size_t inside = flashOffset - g_window.offset;
            size_t amount = g_window.length - inside;
            if (amount > length) amount = length;
            ok = callback(g_window.pointer + inside, amount, logicalOffset,
                          context);
            flashOffset += amount;
            logicalOffset += amount;
            length -= amount;
        } else {
            size_t amount = length > sizeof(fallback) ? sizeof(fallback)
                                                     : length;
            if (esp_partition_read(g_partition, flashOffset, fallback,
                                   amount) != ESP_OK) {
                ok = false;
            } else {
                ok = callback(fallback, amount, logicalOffset, context);
            }
            flashOffset += amount;
            logicalOffset += amount;
            length -= amount;
        }
    }
    giveFlash();
    return ok;
}

bool readDataHeader(uint16_t sector, DataHeader& header) {
    return sector < g_dataSectorCount &&
           flashRead(dataOffset(sector), &header, sizeof(header));
}

bool validHeaderOnly(const DataHeader& header) {
    return header.magic == DATA_MAGIC && header.version == RECORD_VERSION &&
           header.recordType == DATA_OBJECT_PAGE &&
           header.commitWord == COMMIT_WORD &&
           header.logicalPage < g_dataSectorCount &&
           header.payloadLength <= PAGE_PAYLOAD_SIZE &&
           header.generationInverse == ~header.generation &&
           header.headerCrc32 == headerCrc(header);
}

bool sectorFullyErased(uint16_t sector) {
    uint8_t bytes[64];
    const size_t base = dataOffset(sector);
    for (size_t offset = 0; offset < PAGE_SIZE; offset += sizeof(bytes)) {
        if (!flashRead(base + offset, bytes, sizeof(bytes)) ||
            !allErased(bytes, sizeof(bytes))) {
            return false;
        }
    }
    return true;
}

bool verifyPayload(uint16_t sector, const DataHeader& header) {
    uint8_t bytes[256];
    uint32_t crc = 0xFFFFFFFFU;
    size_t remaining = PAGE_PAYLOAD_SIZE;
    size_t offset = dataOffset(sector) + PAGE_HEADER_SIZE;
    while (remaining) {
        const size_t amount = remaining > sizeof(bytes) ? sizeof(bytes)
                                                       : remaining;
        if (!flashRead(offset, bytes, amount)) return false;
        crc = crcUpdate(crc, bytes, amount);
        offset += amount;
        remaining -= amount;
    }
    return (crc ^ 0xFFFFFFFFU) == header.payloadCrc32;
}

bool validControl(const ControlRecord& record) {
    return record.magic == CONTROL_MAGIC &&
           record.version == RECORD_VERSION &&
           record.recordType == CONTROL_OBJECT_STATE &&
           record.commitWord == COMMIT_WORD &&
           record.crc32 == controlCrc(record);
}

bool validSuper(const Superblock& superblock) {
    return superblock.magic == SUPER_MAGIC &&
           superblock.version == FORMAT_VERSION &&
           superblock.headerBytes == sizeof(Superblock) &&
           superblock.sectorBytes == PAGE_SIZE &&
           superblock.payloadBytes == PAGE_PAYLOAD_SIZE &&
           superblock.partitionBytes == g_partition->size &&
           superblock.dataFirstSector == DATA_FIRST_SECTOR &&
           superblock.dataSectorCount > 0 &&
           superblock.dataSectorCount <= MAX_DATA_SECTORS &&
           superblock.commitWord == COMMIT_WORD &&
           superblock.crc32 == superCrc(superblock);
}

int findObject(uint64_t hash, uint32_t tag) {
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        if (g_objects[i].used && g_objects[i].storeHash == hash &&
            g_objects[i].keyTag == tag) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int findFreeObject() {
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        if (!g_objects[i].used && !g_objects[i].reserved)
            return static_cast<int>(i);
    }
    return -1;
}

StorageClass classFromFlags(uint16_t flags) {
    const uint16_t value = flags & CONTROL_CLASS_MASK;
    if (value == static_cast<uint16_t>(StorageClass::Durable))
        return StorageClass::Durable;
    if (value == static_cast<uint16_t>(StorageClass::Cache))
        return StorageClass::Cache;
    return StorageClass::Volatile;
}

uint16_t makeFlags(StorageClass storageClass, bool deleted) {
    return static_cast<uint16_t>(storageClass) |
           (deleted ? CONTROL_FLAG_DELETED : 0);
}

ControlRecord recordFromState(const ObjectState& object) {
    ControlRecord record = {};
    record.magic = CONTROL_MAGIC;
    record.version = RECORD_VERSION;
    record.recordType = CONTROL_OBJECT_STATE;
    record.flags = object.flags;
    record.storeHash = object.storeHash;
    record.keyTag = object.keyTag;
    record.activeEpoch = object.activeEpoch;
    record.logicalSize = object.logicalSize;
    record.quota = object.quota;
    record.sequence = object.sequence;
    record.transactionId = object.transactionId;
    record.commitWord = COMMIT_WORD;
    record.crc32 = controlCrc(record);
    return record;
}

void applyControlRecord(const ControlRecord& record) {
    int slot = findObject(record.storeHash, record.keyTag);
    if (slot < 0) {
        slot = findFreeObject();
        if (slot < 0) return;
        g_objects[slot] = {};
        g_objects[slot].used = true;
        g_objects[slot].storeHash = record.storeHash;
        g_objects[slot].keyTag = record.keyTag;
    } else if (!newer(record.sequence, g_objects[slot].sequence) &&
               record.sequence != g_objects[slot].sequence) {
        return;
    }
    ObjectState& object = g_objects[slot];
    object.flags = record.flags;
    object.deleted = (record.flags & CONTROL_FLAG_DELETED) != 0;
    object.activeEpoch = record.activeEpoch;
    object.logicalSize = record.logicalSize;
    object.quota = record.quota;
    object.sequence = record.sequence;
    object.transactionId = record.transactionId;
    if (newer(record.sequence, g_controlSequence))
        g_controlSequence = record.sequence;
    if (newer(record.transactionId, g_transactionSequence))
        g_transactionSequence = record.transactionId;
}

size_t indexStart(uint64_t hash, uint32_t tag, uint16_t logicalPage) {
    return static_cast<size_t>(
               mix64(hash ^ (uint64_t(tag) << 17U) ^ logicalPage)) &
           (INDEX_BUCKETS - 1U);
}

bool headerMatches(uint16_t sector, uint64_t hash, uint32_t tag,
                   uint16_t logicalPage) {
    DataHeader header;
    return readDataHeader(sector, header) && validHeaderOnly(header) &&
           header.storeHash == hash && header.keyTag == tag &&
           header.logicalPage == logicalPage;
}

bool findIndexBucket(uint64_t hash, uint32_t tag, uint16_t logicalPage,
                     size_t& bucket, bool& found) {
    const size_t start = indexStart(hash, tag, logicalPage);
    size_t firstTombstone = INDEX_BUCKETS;
    for (size_t probe = 0; probe < INDEX_BUCKETS; ++probe) {
        const size_t current = (start + probe) & (INDEX_BUCKETS - 1U);
        const uint16_t value = g_index[current];
        if (value == INDEX_EMPTY) {
            bucket = firstTombstone < INDEX_BUCKETS ? firstTombstone : current;
            found = false;
            return true;
        }
        if (value == INDEX_TOMBSTONE) {
            if (firstTombstone == INDEX_BUCKETS) firstTombstone = current;
            continue;
        }
        const uint16_t sector = value - 1U;
        if (sector < g_dataSectorCount &&
            headerMatches(sector, hash, tag, logicalPage)) {
            bucket = current;
            found = true;
            return true;
        }
    }
    if (firstTombstone < INDEX_BUCKETS) {
        bucket = firstTombstone;
        found = false;
        return true;
    }
    return false;
}

bool indexGet(uint64_t hash, uint32_t tag, uint16_t logicalPage,
              uint16_t& sector) {
    size_t bucket = 0;
    bool found = false;
    if (!findIndexBucket(hash, tag, logicalPage, bucket, found) || !found)
        return false;
    sector = g_index[bucket] - 1U;
    return true;
}

bool indexSet(uint64_t hash, uint32_t tag, uint16_t logicalPage,
              uint16_t sector, uint16_t* previous = nullptr) {
    size_t bucket = 0;
    bool found = false;
    if (!findIndexBucket(hash, tag, logicalPage, bucket, found)) return false;
    if (previous) *previous = found ? g_index[bucket] - 1U : INDEX_TOMBSTONE;
    if (!found && g_index[bucket] == INDEX_TOMBSTONE && g_indexTombstones)
        --g_indexTombstones;
    g_index[bucket] = sector + 1U;
    return true;
}

void indexRemoveBucket(size_t bucket) {
    if (bucket >= INDEX_BUCKETS || g_index[bucket] == INDEX_EMPTY ||
        g_index[bucket] == INDEX_TOMBSTONE)
        return;
    g_index[bucket] = INDEX_TOMBSTONE;
    ++g_indexTombstones;
}

void rebuildIndex() {
    memset(g_index, 0, INDEX_BUCKETS * sizeof(uint16_t));
    g_indexTombstones = 0;
    for (uint16_t sector = 0; sector < g_dataSectorCount; ++sector) {
        if (g_sectorState[sector] != SectorState::Live) continue;
        DataHeader header;
        if (!readDataHeader(sector, header) || !validHeaderOnly(header) ||
            !indexSet(header.storeHash, header.keyTag, header.logicalPage,
                      sector)) {
            g_sectorState[sector] = SectorState::Stale;
        }
    }
}

void removeObjectPages(uint64_t hash, uint32_t tag,
                       size_t firstPageToRemove = 0) {
    for (size_t bucket = 0; bucket < INDEX_BUCKETS; ++bucket) {
        const uint16_t value = g_index[bucket];
        if (value == INDEX_EMPTY || value == INDEX_TOMBSTONE) continue;
        const uint16_t sector = value - 1U;
        DataHeader header;
        if (!readDataHeader(sector, header) || !validHeaderOnly(header))
            continue;
        if (header.storeHash == hash && header.keyTag == tag &&
            header.logicalPage >= firstPageToRemove) {
            if (g_sectorState[sector] == SectorState::Live)
                g_sectorState[sector] = SectorState::Stale;
            indexRemoveBucket(bucket);
        }
    }
    if (g_indexTombstones > INDEX_BUCKETS / 8U) rebuildIndex();
}

bool loadSuperblock() {
    Superblock first = {};
    Superblock second = {};
    const bool firstOk = flashRead(0, &first, sizeof(first)) && validSuper(first);
    const bool secondOk =
        flashRead(PAGE_SIZE, &second, sizeof(second)) && validSuper(second);
    if (!firstOk && !secondOk) return false;
    if (firstOk && secondOk)
        g_superblock = newer(second.formatGeneration, first.formatGeneration)
                           ? second
                           : first;
    else
        g_superblock = firstOk ? first : second;
    g_dataSectorCount = static_cast<uint16_t>(g_superblock.dataSectorCount);
    return true;
}

bool writeCommittedBlockLocked(size_t offset, const void* block) {
    const uint8_t* bytes = static_cast<const uint8_t*>(block);
    if (esp_partition_write(g_partition, offset, bytes,
                            PAGE_HEADER_SIZE - sizeof(uint32_t)) != ESP_OK)
        return false;
    return esp_partition_write(
               g_partition, offset + PAGE_HEADER_SIZE - sizeof(uint32_t),
               bytes + PAGE_HEADER_SIZE - sizeof(uint32_t),
               sizeof(uint32_t)) == ESP_OK;
}

bool formatPartition() {
    if (!takeFlash()) return false;
    unmapWindowLocked();
    bool ok = esp_partition_erase_range(g_partition, 0, g_partition->size) ==
              ESP_OK;
    Superblock superblock = {};
    if (ok) {
        superblock.magic = SUPER_MAGIC;
        superblock.version = FORMAT_VERSION;
        superblock.headerBytes = sizeof(Superblock);
        superblock.sectorBytes = PAGE_SIZE;
        superblock.payloadBytes = PAGE_PAYLOAD_SIZE;
        superblock.partitionBytes = g_partition->size;
        superblock.dataFirstSector = DATA_FIRST_SECTOR;
        superblock.dataSectorCount =
            g_partition->size / PAGE_SIZE - DATA_FIRST_SECTOR;
        superblock.formatGeneration = esp_random();
        esp_fill_random(superblock.sipKey, sizeof(superblock.sipKey));
        superblock.uuid = (uint64_t(esp_random()) << 32U) | esp_random();
        superblock.commitWord = COMMIT_WORD;
        superblock.crc32 = superCrc(superblock);
        ok = writeCommittedBlockLocked(0, &superblock) &&
             writeCommittedBlockLocked(PAGE_SIZE, &superblock);
    }
    giveFlash();
    if (ok) {
        g_superblock = superblock;
        g_dataSectorCount =
            static_cast<uint16_t>(superblock.dataSectorCount);
    }
    return ok;
}

bool scanJournal() {
    memset(g_objects, 0, sizeof(g_objects));
    memset(g_journalNext, 0, sizeof(g_journalNext));
    memset(g_journalErased, 0, sizeof(g_journalErased));
    g_journalActive = 0xFF;
    g_controlSequence = 0;
    g_transactionSequence = 0;

    uint32_t activeSequence = 0;
    for (uint8_t sector = 0; sector < JOURNAL_SECTORS; ++sector) {
        bool erased = true;
        uint32_t sectorSequence = 0;
        uint8_t next = 0;
        for (uint8_t slot = 0; slot < JOURNAL_RECORDS_PER_SECTOR; ++slot) {
            ControlRecord record;
            if (!flashRead(journalOffset(sector, slot), &record,
                           sizeof(record)))
                return false;
            if (allErased(&record, sizeof(record))) continue;
            erased = false;
            next = slot + 1U;
            if (!validControl(record)) continue;
            if (newer(record.sequence, sectorSequence))
                sectorSequence = record.sequence;
            applyControlRecord(record);
        }
        g_journalErased[sector] = erased;
        g_journalNext[sector] = erased ? 0 : next;
        if (!erased && (g_journalActive == 0xFF ||
                        newer(sectorSequence, activeSequence))) {
            g_journalActive = sector;
            activeSequence = sectorSequence;
        }
    }

    // Volatile objects intentionally disappear on every boot. Their old
    // record is harmless; data scan below treats these pages as stale.
    for (ObjectState& object : g_objects) {
        if (object.used && !object.deleted &&
            classFromFlags(object.flags) == StorageClass::Volatile) {
            object.deleted = true;
            object.flags |= CONTROL_FLAG_DELETED;
            object.logicalSize = 0;
        }
    }
    return true;
}

bool scanData() {
    memset(g_sectorState, 0,
           g_dataSectorCount * sizeof(SectorState));
    memset(g_sectorPins, 0, g_dataSectorCount * sizeof(uint8_t));
    memset(g_index, 0, INDEX_BUCKETS * sizeof(uint16_t));
    g_indexTombstones = 0;
    g_dataGeneration = 0;

    for (uint16_t sector = 0; sector < g_dataSectorCount; ++sector) {
        DataHeader header;
        if (!readDataHeader(sector, header)) return false;
        if (allErased(&header, sizeof(header))) {
            g_sectorState[sector] = sectorFullyErased(sector)
                                        ? SectorState::Erased
                                        : SectorState::Stale;
            continue;
        }
        if (!validHeaderOnly(header) || !verifyPayload(sector, header)) {
            g_sectorState[sector] = SectorState::Stale;
            continue;
        }
        if (newer(header.generation, g_dataGeneration))
            g_dataGeneration = header.generation;

        const int objectSlot = findObject(header.storeHash, header.keyTag);
        if (objectSlot < 0 || g_objects[objectSlot].deleted ||
            header.objectEpoch > g_objects[objectSlot].activeEpoch ||
            size_t(header.logicalPage) * PAGE_PAYLOAD_SIZE >=
                g_objects[objectSlot].logicalSize) {
            g_sectorState[sector] = SectorState::Stale;
            continue;
        }

        uint16_t previous = 0;
        if (indexGet(header.storeHash, header.keyTag, header.logicalPage,
                     previous)) {
            DataHeader oldHeader;
            if (!readDataHeader(previous, oldHeader) ||
                header.objectEpoch > oldHeader.objectEpoch ||
                (header.objectEpoch == oldHeader.objectEpoch &&
                 newer(header.generation, oldHeader.generation))) {
                g_sectorState[previous] = SectorState::Stale;
                indexSet(header.storeHash, header.keyTag, header.logicalPage,
                         sector);
                g_sectorState[sector] = SectorState::Live;
            } else {
                g_sectorState[sector] = SectorState::Stale;
            }
        } else if (indexSet(header.storeHash, header.keyTag,
                            header.logicalPage, sector)) {
            g_sectorState[sector] = SectorState::Live;
        } else {
            setStatus(Status::IndexFull);
            return false;
        }
    }
    return true;
}

bool writeControlAtLocked(uint8_t sector, uint8_t slot,
                          const ControlRecord& record) {
    return writeCommittedBlockLocked(journalOffset(sector, slot), &record);
}

bool compactJournalLocked(uint8_t target) {
    if (target >= JOURNAL_SECTORS || !g_journalErased[target]) return false;
    uint8_t slot = 0;
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        if (!g_objects[i].used) continue;
        if (slot >= JOURNAL_RECORDS_PER_SECTOR) return false;
        const ControlRecord snapshot = recordFromState(g_objects[i]);
        if (!writeControlAtLocked(target, slot, snapshot)) {
            g_journalErased[target] = false;
            g_journalNext[target] = slot + 1U;
            return false;
        }
        ++slot;
    }

    g_journalErased[target] = false;
    g_journalNext[target] = slot;
    for (uint8_t sector = 0; sector < JOURNAL_SECTORS; ++sector) {
        if (sector == target) continue;
        const size_t offset = (SUPERBLOCK_SECTORS + sector) * PAGE_SIZE;
        if (esp_partition_erase_range(g_partition, offset, PAGE_SIZE) != ESP_OK)
            return false;
        g_journalErased[sector] = true;
        g_journalNext[sector] = 0;
    }
    g_journalActive = target;
    return true;
}

bool chooseJournalSlotLocked(uint8_t& sector, uint8_t& slot) {
    if (g_journalActive < JOURNAL_SECTORS &&
        !g_journalErased[g_journalActive] &&
        g_journalNext[g_journalActive] < JOURNAL_RECORDS_PER_SECTOR) {
        sector = g_journalActive;
        slot = g_journalNext[sector];
        return true;
    }
    for (uint8_t candidate = 0; candidate < JOURNAL_SECTORS; ++candidate) {
        if (!g_journalErased[candidate] &&
            g_journalNext[candidate] < JOURNAL_RECORDS_PER_SECTOR) {
            g_journalActive = candidate;
            sector = candidate;
            slot = g_journalNext[candidate];
            return true;
        }
    }

    uint8_t erasedCount = 0;
    uint8_t erasedTarget = 0xFF;
    for (uint8_t candidate = 0; candidate < JOURNAL_SECTORS; ++candidate) {
        if (g_journalErased[candidate]) {
            ++erasedCount;
            if (erasedTarget == 0xFF) erasedTarget = candidate;
        }
    }
    if (erasedTarget == 0xFF) return false;
    if (erasedCount == 1 && !compactJournalLocked(erasedTarget)) return false;
    g_journalActive = erasedTarget;
    sector = erasedTarget;
    slot = g_journalNext[erasedTarget];
    return slot < JOURNAL_RECORDS_PER_SECTOR;
}

Status appendControlPhysical(const ControlRecord& input) {
    if (!takeFlash()) return Status::IoError;
    unmapWindowLocked();
    ControlRecord record = input;
    record.commitWord = COMMIT_WORD;
    record.crc32 = controlCrc(record);
    uint8_t sector = 0;
    uint8_t slot = 0;
    bool ok = chooseJournalSlotLocked(sector, slot);
    if (ok) {
        ok = writeControlAtLocked(sector, slot, record);
        g_journalErased[sector] = false;
        g_journalNext[sector] = slot + 1U;
        g_journalActive = sector;
    }
    giveFlash();
    return ok ? Status::Ok : Status::IoError;
}

Status writeDataPhysical(const WriteJob& job) {
    if (!job.image || job.targetSector >= g_dataSectorCount || !takeFlash())
        return Status::IoError;
    unmapWindowLocked();
    const size_t offset = dataOffset(job.targetSector);
    const DataHeader* header =
        reinterpret_cast<const DataHeader*>(job.image);
    bool ok = esp_partition_write(g_partition, offset + PAGE_HEADER_SIZE,
                                  job.image + PAGE_HEADER_SIZE,
                                  PAGE_PAYLOAD_SIZE) == ESP_OK;
    if (ok) {
        ok = esp_partition_write(g_partition, offset, job.image,
                                 PAGE_HEADER_SIZE - sizeof(uint32_t)) == ESP_OK;
    }
    if (ok) {
        ok = esp_partition_write(
                 g_partition, offset + PAGE_HEADER_SIZE - sizeof(uint32_t),
                 &header->commitWord, sizeof(header->commitWord)) == ESP_OK;
    }
    giveFlash();
    return ok ? Status::Ok : Status::IoError;
}

Status eraseDataPhysical(uint16_t sector) {
    if (sector >= g_dataSectorCount || !takeFlash()) return Status::IoError;
    unmapWindowLocked();
    const bool ok = esp_partition_erase_range(g_partition, dataOffset(sector),
                                               PAGE_SIZE) == ESP_OK;
    giveFlash();
    return ok ? Status::Ok : Status::IoError;
}

void notifyWorker() {
    if (g_workerTask) xTaskNotifyGive(g_workerTask);
}

void clearTransactionLocked() {
    g_transaction.active = false;
    g_transaction.failed = false;
    g_transaction.objectSlot = -1;
    g_transaction.ownerToken = 0;
    g_transaction.candidateEpoch = 0;
    g_transaction.transactionId = 0;
    g_transaction.logicalSize = 0;
    g_transaction.failure = Status::Ok;
    if (g_transaction.pendingByPage) {
        memset(g_transaction.pendingByPage, 0,
               g_dataSectorCount * sizeof(uint16_t));
    }
}

void reapJobsLocked() {
    for (uint8_t i = 0; i < g_jobCount; ++i) {
        WriteJob& job = g_jobs[i];
        if (job.state != JobState::Done && job.state != JobState::Failed)
            continue;
        const bool belongs =
            g_transaction.active &&
            job.transactionId == g_transaction.transactionId &&
            job.ownerToken == g_transaction.ownerToken &&
            job.objectSlot == g_transaction.objectSlot;
        if (job.state == JobState::Done && belongs &&
            job.logicalPage < g_dataSectorCount) {
            const uint16_t oldValue =
                g_transaction.pendingByPage[job.logicalPage];
            if (oldValue) {
                const uint16_t oldSector = oldValue - 1U;
                if (g_sectorState[oldSector] == SectorState::Pending)
                    g_sectorState[oldSector] = SectorState::Stale;
            }
            g_transaction.pendingByPage[job.logicalPage] =
                job.targetSector + 1U;
            g_sectorState[job.targetSector] = SectorState::Pending;
        } else {
            if (job.targetSector < g_dataSectorCount)
                g_sectorState[job.targetSector] = SectorState::Stale;
            if (belongs) {
                g_transaction.failed = true;
                g_transaction.failure = job.result;
            }
        }
        job.state = JobState::Free;
        job.objectSlot = -1;
        job.result = Status::Ok;
    }
}

bool serviceOneWork() {
    WriteJob* selectedJob = nullptr;
    CommandKind commandKind = CommandKind::None;
    ControlRecord commandRecord = {};
    uint16_t commandSector = 0;

    if (!takeState()) return false;
    reapJobsLocked();
    if (g_criticalDepth > 0 || g_flashBusy) {
        giveState();
        return false;
    }

    uint32_t oldestSequence = 0;
    for (uint8_t i = 0; i < g_jobCount; ++i) {
        WriteJob& job = g_jobs[i];
        if (job.state != JobState::Queued) continue;
        if (!selectedJob || newer(oldestSequence, job.queueSequence)) {
            selectedJob = &job;
            oldestSequence = job.queueSequence;
        }
    }
    if (selectedJob) {
        selectedJob->state = JobState::Writing;
        g_sectorState[selectedJob->targetSector] = SectorState::Writing;
    } else if (g_command.state == CommandState::Pending) {
        g_command.state = CommandState::Running;
        commandKind = g_command.kind;
        commandRecord = g_command.record;
        commandSector = g_command.dataSector;
    } else {
        giveState();
        return false;
    }
    g_flashBusy = true;
    giveState();

    Status result = Status::IoError;
    if (selectedJob)
        result = writeDataPhysical(*selectedJob);
    else if (commandKind == CommandKind::AppendControl)
        result = appendControlPhysical(commandRecord);
    else if (commandKind == CommandKind::EraseData)
        result = eraseDataPhysical(commandSector);

    takeState();
    g_flashBusy = false;
    if (selectedJob) {
        selectedJob->result = result;
        selectedJob->state =
            result == Status::Ok ? JobState::Done : JobState::Failed;
        g_sectorState[selectedJob->targetSector] =
            result == Status::Ok ? SectorState::Pending : SectorState::Stale;
    } else {
        if (commandKind == CommandKind::EraseData &&
            commandSector < g_dataSectorCount) {
            g_sectorState[commandSector] =
                result == Status::Ok ? SectorState::Erased : SectorState::Bad;
        }
        g_command.result = result;
        g_command.state = CommandState::Done;
    }
    giveState();
    return true;
}

size_t countState(SectorState requested) {
    size_t count = 0;
    for (uint16_t i = 0; i < g_dataSectorCount; ++i)
        if (g_sectorState[i] == requested) ++count;
    return count;
}

bool serviceOneGc() {
    uint16_t selected = INDEX_TOMBSTONE;
    if (!takeState()) return false;
    if (g_criticalDepth > 0 || g_flashBusy ||
        countState(SectorState::Erased) >= ERASED_TARGET) {
        giveState();
        return false;
    }
    for (uint16_t probe = 0; probe < g_dataSectorCount; ++probe) {
        const uint16_t candidate =
            static_cast<uint16_t>((g_gcCursor + probe) % g_dataSectorCount);
        if (g_sectorState[candidate] == SectorState::Stale &&
            g_sectorPins[candidate] == 0) {
            selected = candidate;
            g_gcCursor = static_cast<uint16_t>((candidate + 1) %
                                               g_dataSectorCount);
            g_sectorState[candidate] = SectorState::Erasing;
            g_flashBusy = true;
            break;
        }
    }
    giveState();
    if (selected == INDEX_TOMBSTONE) return false;

    const Status result = eraseDataPhysical(selected);
    takeState();
    g_flashBusy = false;
    g_sectorState[selected] =
        result == Status::Ok ? SectorState::Erased : SectorState::Bad;
    giveState();
    return true;
}

void workerMain(void*) {
    while (true) {
        if (serviceOneWork()) continue;
        if (serviceOneGc()) continue;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
    }
}

bool waitCommand(uint32_t timeoutMs, Status& result) {
    const uint32_t started = millis();
    while (true) {
        if (g_cooperativeWriter) serviceOneWork();
        if (!takeState(pdMS_TO_TICKS(20))) continue;
        if (g_command.state == CommandState::Done) {
            result = g_command.result;
            g_command = {};
            giveState();
            return result == Status::Ok;
        }
        giveState();
        if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
            result = Status::Timeout;
            return false;
        }
        delay(1);
    }
}

bool submitControl(const ControlRecord& record, uint32_t timeoutMs) {
    if (!takeState()) return false;
    if (g_criticalDepth > 0 || g_command.state != CommandState::Idle) {
        giveState();
        setStatus(Status::Busy);
        return false;
    }
    g_command.kind = CommandKind::AppendControl;
    g_command.record = record;
    g_command.result = Status::Ok;
    g_command.state = CommandState::Pending;
    giveState();
    notifyWorker();
    Status result;
    const bool ok = waitCommand(timeoutMs, result);
    setStatus(ok ? Status::Ok : result);
    return ok;
}

bool submitErase(uint16_t sector, uint32_t timeoutMs) {
    if (!takeState()) return false;
    if (g_criticalDepth > 0 || g_command.state != CommandState::Idle ||
        sector >= g_dataSectorCount ||
        g_sectorState[sector] != SectorState::Erasing) {
        giveState();
        setStatus(Status::Busy);
        return false;
    }
    g_command.kind = CommandKind::EraseData;
    g_command.dataSector = sector;
    g_command.result = Status::Ok;
    g_command.state = CommandState::Pending;
    giveState();
    notifyWorker();
    Status result;
    const bool ok = waitCommand(timeoutMs, result);
    setStatus(ok ? Status::Ok : result);
    return ok;
}

bool eraseAbandonedEpoch(uint64_t hash, uint32_t tag, uint32_t epoch,
                         uint32_t timeoutMs) {
    for (uint16_t sector = 0; sector < g_dataSectorCount; ++sector) {
        bool pinned = false;
        if (takeState()) {
            if (g_sectorState[sector] == SectorState::Stale) {
                ++g_sectorPins[sector];
                pinned = true;
            }
            giveState();
        }
        if (!pinned) continue;
        DataHeader header;
        const bool matches = readDataHeader(sector, header) &&
                             validHeaderOnly(header) &&
                             header.storeHash == hash &&
                             header.keyTag == tag &&
                             header.objectEpoch == epoch;
        bool erase = false;
        takeState();
        if (g_sectorPins[sector]) --g_sectorPins[sector];
        if (matches && g_sectorPins[sector] == 0 &&
            g_sectorState[sector] == SectorState::Stale) {
            g_sectorState[sector] = SectorState::Erasing;
            erase = true;
        }
        giveState();
        if (erase && !submitErase(sector, timeoutMs)) return false;
    }
    return true;
}

bool waitForJobs(uint32_t ownerToken, uint32_t transactionId,
                 uint32_t timeoutMs) {
    const uint32_t started = millis();
    while (true) {
        if (g_cooperativeWriter) serviceOneWork();
        bool waiting = false;
        bool failed = false;
        Status failure = Status::Ok;
        if (!takeState(pdMS_TO_TICKS(20))) continue;
        reapJobsLocked();
        for (uint8_t i = 0; i < g_jobCount; ++i) {
            const WriteJob& job = g_jobs[i];
            if (job.ownerToken == ownerToken &&
                job.transactionId == transactionId &&
                job.state != JobState::Free) {
                waiting = true;
                break;
            }
        }
        if (g_transaction.active && g_transaction.ownerToken == ownerToken &&
            g_transaction.transactionId == transactionId &&
            g_transaction.failed) {
            failed = true;
            failure = g_transaction.failure;
        }
        giveState();
        if (failed) {
            setStatus(failure == Status::Ok ? Status::TransactionFailed
                                            : failure);
            return false;
        }
        if (!waiting) {
            setStatus(Status::Ok);
            return true;
        }
        if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
            setStatus(Status::Timeout);
            return false;
        }
        notifyWorker();
        delay(1);
    }
}

bool pinReadSector(uint64_t hash, uint32_t tag, uint16_t logicalPage,
                   uint32_t ownerToken, uint16_t& sector) {
    if (!takeState()) return false;
    reapJobsLocked();
    bool found = false;
    if (g_transaction.active && g_transaction.ownerToken == ownerToken &&
        logicalPage < g_dataSectorCount &&
        g_transaction.pendingByPage[logicalPage]) {
        sector = g_transaction.pendingByPage[logicalPage] - 1U;
        found = true;
    } else {
        found = indexGet(hash, tag, logicalPage, sector);
    }
    if (found && sector < g_dataSectorCount &&
        g_sectorState[sector] != SectorState::Erasing &&
        g_sectorState[sector] != SectorState::Bad) {
        ++g_sectorPins[sector];
    } else {
        found = false;
    }
    giveState();
    return found;
}

void unpinSector(uint16_t sector) {
    if (!takeState()) return;
    if (sector < g_dataSectorCount && g_sectorPins[sector])
        --g_sectorPins[sector];
    giveState();
}

bool loadPage(uint16_t sector, uint8_t* payload) {
    memset(payload, 0, PAGE_PAYLOAD_SIZE);
    if (sector == INDEX_TOMBSTONE) return true;
    DataHeader header;
    if (!readDataHeader(sector, header) || !validHeaderOnly(header)) return false;
    if (!header.payloadLength) return true;
    return flashRead(dataOffset(sector) + PAGE_HEADER_SIZE, payload,
                     header.payloadLength);
}

void updateJobHeader(WriteJob& job, const ObjectState& object,
                     size_t logicalSize) {
    DataHeader* header = reinterpret_cast<DataHeader*>(job.image);
    header->magic = DATA_MAGIC;
    header->version = RECORD_VERSION;
    header->recordType = DATA_OBJECT_PAGE;
    header->flags = 0;
    header->storeHash = object.storeHash;
    header->objectEpoch = g_transaction.candidateEpoch;
    header->logicalPage = job.logicalPage;
    const size_t pageStart = size_t(job.logicalPage) * PAGE_PAYLOAD_SIZE;
    size_t payloadLength = logicalSize > pageStart ? logicalSize - pageStart : 0;
    if (payloadLength > PAGE_PAYLOAD_SIZE) payloadLength = PAGE_PAYLOAD_SIZE;
    header->payloadLength = static_cast<uint16_t>(payloadLength);
    header->logicalSize = static_cast<uint32_t>(logicalSize);
    header->transactionId = g_transaction.transactionId;
    header->payloadCrc32 =
        crc32(job.image + PAGE_HEADER_SIZE, PAGE_PAYLOAD_SIZE);
    header->generationInverse = ~header->generation;
    header->keyTag = object.keyTag;
    memset(header->reserved, 0, sizeof(header->reserved));
    header->commitWord = COMMIT_WORD;
    header->headerCrc32 = headerCrc(*header);
}

int findFreeJobLocked() {
    for (uint8_t i = 0; i < g_jobCount; ++i)
        if (g_jobs[i].state == JobState::Free) return i;
    return -1;
}

int findErasedSectorLocked() {
    for (uint16_t probe = 0; probe < g_dataSectorCount; ++probe) {
        const uint16_t candidate =
            static_cast<uint16_t>((g_allocCursor + probe) % g_dataSectorCount);
        if (g_sectorState[candidate] == SectorState::Erased) {
            g_allocCursor = static_cast<uint16_t>((candidate + 1) %
                                                  g_dataSectorCount);
            return candidate;
        }
    }
    return -1;
}

bool queuePageWrite(int16_t objectSlot, uint32_t ownerToken,
                    uint16_t logicalPage, size_t pageOffset,
                    const uint8_t* source, size_t length, bool nonBlocking) {
    const uint32_t waitStarted = millis();
    while (true) {
        if (g_cooperativeWriter) serviceOneWork();
        if (!takeState()) return false;
        reapJobsLocked();
        if (!g_transaction.active ||
            g_transaction.objectSlot != objectSlot ||
            g_transaction.ownerToken != ownerToken) {
            giveState();
            setStatus(Status::Busy);
            return false;
        }

        for (uint8_t i = 0; i < g_jobCount; ++i) {
            WriteJob& job = g_jobs[i];
            if (job.ownerToken != ownerToken ||
                job.transactionId != g_transaction.transactionId ||
                job.logicalPage != logicalPage)
                continue;
            if (job.state == JobState::Queued) {
                memcpy(job.image + PAGE_HEADER_SIZE + pageOffset, source,
                       length);
                updateJobHeader(job, g_objects[objectSlot],
                                g_transaction.logicalSize);
                giveState();
                notifyWorker();
                setStatus(Status::Ok);
                return true;
            }
            if (job.state == JobState::Writing ||
                job.state == JobState::Filling) {
                giveState();
                if (nonBlocking) {
                    setStatus(Status::WouldBlock);
                    return false;
                }
                notifyWorker();
                delay(1);
                continue;
            }
        }

        const int jobIndex = findFreeJobLocked();
        const int target = findErasedSectorLocked();
        if (jobIndex < 0 || target < 0) {
            if (target >= 0) {
                // No sector was consumed: findErasedSectorLocked only locates.
            }
            const bool hasStale = countState(SectorState::Stale) > 0;
            giveState();
            if (nonBlocking) {
                setStatus(Status::WouldBlock);
                return false;
            }
            if (target < 0 && !hasStale) {
                setStatus(Status::NoSpace);
                return false;
            }
            if (static_cast<uint32_t>(millis() - waitStarted) >=
                DEFAULT_OPERATION_TIMEOUT_MS) {
                setStatus(target < 0 ? Status::NoSpace : Status::Timeout);
                return false;
            }
            notifyWorker();
            delay(1);
            continue;
        }

        WriteJob& job = g_jobs[jobIndex];
        job.state = JobState::Filling;
        job.ownerToken = ownerToken;
        job.transactionId = g_transaction.transactionId;
        job.targetSector = static_cast<uint16_t>(target);
        job.logicalPage = logicalPage;
        job.objectSlot = objectSlot;
        job.result = Status::Ok;
        g_sectorState[target] = SectorState::Reserved;

        uint16_t sourceSector = INDEX_TOMBSTONE;
        bool sourcePinned = false;
        const uint16_t pending = g_transaction.pendingByPage[logicalPage];
        if (pending) {
            sourceSector = pending - 1U;
            ++g_sectorPins[sourceSector];
            sourcePinned = true;
        } else if (indexGet(g_objects[objectSlot].storeHash,
                            g_objects[objectSlot].keyTag, logicalPage,
                            sourceSector)) {
            ++g_sectorPins[sourceSector];
            sourcePinned = true;
        }
        const uint32_t generation = ++g_dataGeneration;
        giveState();

        memset(job.image, 0xFF, PAGE_HEADER_SIZE);
        bool loaded = loadPage(sourceSector,
                               job.image + PAGE_HEADER_SIZE);
        if (sourcePinned) unpinSector(sourceSector);
        if (loaded)
            memcpy(job.image + PAGE_HEADER_SIZE + pageOffset, source, length);

        takeState();
        if (!loaded || !g_transaction.active ||
            g_transaction.objectSlot != objectSlot ||
            g_transaction.ownerToken != ownerToken ||
            job.state != JobState::Filling) {
            g_sectorState[target] = SectorState::Stale;
            job.state = JobState::Free;
            giveState();
            setStatus(loaded ? Status::Busy : Status::IoError);
            return false;
        }
        DataHeader* header = reinterpret_cast<DataHeader*>(job.image);
        header->generation = generation;
        updateJobHeader(job, g_objects[objectSlot],
                        g_transaction.logicalSize);
        job.queueSequence = ++g_jobSequence;
        job.state = JobState::Queued;
        giveState();
        notifyWorker();
        setStatus(Status::Ok);
        return true;
    }
}

bool deleteSlot(int slot, uint32_t timeoutMs) {
    if (slot < 0 || slot >= static_cast<int>(MAX_OBJECTS)) return false;
    ObjectState next;
    if (!takeState()) return false;
    if (!g_objects[slot].used || g_objects[slot].deleted ||
        g_objects[slot].openCount > 0 || g_transaction.active) {
        giveState();
        setStatus(Status::Busy);
        return false;
    }
    next = g_objects[slot];
    next.deleted = true;
    next.flags |= CONTROL_FLAG_DELETED;
    next.logicalSize = 0;
    ++next.activeEpoch;
    next.sequence = ++g_controlSequence;
    next.transactionId = ++g_transactionSequence;
    giveState();
    if (!submitControl(recordFromState(next), timeoutMs)) return false;
    takeState();
    g_objects[slot] = next;
    removeObjectPages(next.storeHash, next.keyTag, 0);
    giveState();
    notifyWorker();
    return true;
}

}  // namespace

bool begin(bool autoFormat) {
    if (g_initialized) return true;
    if (g_beginning) {
        setStatus(Status::Busy);
        return false;
    }
    g_beginning = true;
    g_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           PARTITION_SUBTYPE,
                                           PARTITION_LABEL);
    if (!g_partition)
        g_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               ESP_PARTITION_SUBTYPE_ANY,
                                               PARTITION_LABEL);
    if (!g_partition || g_partition->size / PAGE_SIZE <= DATA_FIRST_SECTOR ||
        g_partition->size > MAX_PARTITION_BYTES) {
        g_beginning = false;
        setStatus(Status::PartitionNotFound);
        return false;
    }
    g_stateMutex = xSemaphoreCreateMutex();
    g_flashMutex = xSemaphoreCreateMutex();
    if (!g_stateMutex || !g_flashMutex) {
        g_beginning = false;
        setStatus(Status::NoMemory);
        return false;
    }

    if (!loadSuperblock()) {
        if (!autoFormat) {
            g_beginning = false;
            setStatus(Status::FormatRequired);
            return false;
        }
        if (!formatPartition() || !loadSuperblock()) {
            g_beginning = false;
            setStatus(Status::FormatFailed);
            return false;
        }
    }
    constexpr size_t sectorStateBytes =
        MAX_DATA_SECTORS * sizeof(SectorState);
    constexpr size_t sectorPinBytes =
        MAX_DATA_SECTORS * sizeof(uint8_t);
    constexpr size_t indexOffset =
        (sectorStateBytes + sectorPinBytes + alignof(uint16_t) - 1U) &
        ~(alignof(uint16_t) - 1U);
    constexpr size_t indexBytes = INDEX_BUCKETS * sizeof(uint16_t);
    constexpr size_t pendingOffset = indexOffset + indexBytes;
    constexpr size_t metadataBytes =
        pendingOffset + MAX_DATA_SECTORS * sizeof(uint16_t);
    g_metadata = static_cast<uint8_t*>(RuntimeMemory::allocateInternal(
        metadataBytes, true, PAGER_INTERNAL_RESERVE));
    if (!g_metadata) {
        g_beginning = false;
        setStatus(Status::NoMemory);
        return false;
    }
    g_sectorState = reinterpret_cast<SectorState*>(g_metadata);
    g_sectorPins = g_metadata + sectorStateBytes;
    g_index = reinterpret_cast<uint16_t*>(g_metadata + indexOffset);
    g_pendingByPage =
        reinterpret_cast<uint16_t*>(g_metadata + pendingOffset);
    g_transaction.pendingByPage = g_pendingByPage;
    if (!scanJournal() || !scanData()) {
        g_beginning = false;
        if (g_lastStatus == Status::NotInitialized) setStatus(Status::Corrupt);
        return false;
    }

    g_jobCount = 0;
    for (uint8_t i = 0; i < WRITE_JOB_LIMIT; ++i) {
        uint8_t* image = static_cast<uint8_t*>(RuntimeMemory::allocateInternal(
            PAGE_SIZE, false, PAGER_INTERNAL_RESERVE));
        if (!image) break;
        g_jobs[i] = {};
        g_jobs[i].image = image;
        ++g_jobCount;
    }
    if (!g_jobCount) {
        g_beginning = false;
        setStatus(Status::NoMemory);
        return false;
    }

    clearTransactionLocked();
    g_command = {};
    g_initialized = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        workerMain, "flash-pager", WORKER_STACK_BYTES, nullptr,
        WORKER_PRIORITY, &g_workerTask, WORKER_CORE);
    if (created != pdPASS) {
        g_workerTask = nullptr;
        g_cooperativeWriter = true;
    }
    g_beginning = false;
    setStatus(Status::Ok);
    return true;
}

bool initialized() { return g_initialized; }

Status lastStatus() { return g_lastStatus; }

const char* statusText(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::NotInitialized: return "not initialized";
        case Status::PartitionNotFound: return "swap partition not found";
        case Status::FormatRequired: return "pager format required";
        case Status::FormatFailed: return "pager format failed";
        case Status::Corrupt: return "pager metadata corrupt";
        case Status::NoMemory: return "pager SRAM allocation failed";
        case Status::IndexFull: return "pager hash index full";
        case Status::ObjectLimit: return "pager object limit reached";
        case Status::NotFound: return "pager object not found";
        case Status::InvalidArgument: return "invalid pager argument";
        case Status::Busy: return "pager busy";
        case Status::WouldBlock: return "pager operation would block";
        case Status::NoSpace: return "pager has no reclaimable space";
        case Status::QuotaExceeded: return "pager object quota exceeded";
        case Status::IoError: return "pager flash I/O error";
        case Status::Timeout: return "pager operation timed out";
        case Status::TransactionFailed: return "pager transaction failed";
        case Status::CallbackAborted: return "pager visit aborted";
        default: return "unknown pager error";
    }
}

void poll() {
    if (!g_initialized) return;
    if (g_cooperativeWriter) {
        if (!serviceOneWork()) serviceOneGc();
    }
    if (takeState(0)) {
        reapJobsLocked();
        giveState();
    }
}

bool flushAll(uint32_t timeoutMs) {
    if (!g_initialized) {
        setStatus(Status::NotInitialized);
        return false;
    }
    const uint32_t started = millis();
    while (true) {
        poll();
        bool pending = false;
        takeState();
        for (uint8_t i = 0; i < g_jobCount; ++i) {
            if (g_jobs[i].state != JobState::Free) {
                pending = true;
                break;
            }
        }
        giveState();
        if (!pending) {
            setStatus(Status::Ok);
            return true;
        }
        if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
            setStatus(Status::Timeout);
            return false;
        }
        notifyWorker();
        delay(1);
    }
}

Stats stats() {
    Stats result;
    result.initialized = g_initialized;
    if (!g_initialized || !takeState()) return result;
    result.workerTask = g_workerTask != nullptr;
    result.cooperativeWriter = g_cooperativeWriter;
    result.partitionBytes = g_partition ? g_partition->size : 0;
    result.dataSectors = g_dataSectorCount;
    result.logicalCapacityBytes = size_t(g_dataSectorCount) * PAGE_PAYLOAD_SIZE;
    result.writeBuffers = g_jobCount;
    result.criticalDepth = g_criticalDepth;
    for (uint16_t i = 0; i < g_dataSectorCount; ++i) {
        switch (g_sectorState[i]) {
            case SectorState::Erased: ++result.erasedSectors; break;
            case SectorState::Live:
                ++result.liveSectors;
                result.physicalLiveBytes += PAGE_PAYLOAD_SIZE;
                break;
            case SectorState::Pending:
            case SectorState::Reserved:
            case SectorState::Writing: ++result.pendingSectors; break;
            case SectorState::Stale:
            case SectorState::Erasing: ++result.staleSectors; break;
            case SectorState::Bad: ++result.badSectors; break;
            default: break;
        }
    }
    for (const ObjectState& object : g_objects) {
        if (!object.used) continue;
        ++result.objectCount;
        result.openObjects += object.openCount;
        if (!object.deleted) result.logicalUsedBytes += object.logicalSize;
    }
    for (uint8_t i = 0; i < g_jobCount; ++i)
        if (g_jobs[i].state == JobState::Queued ||
            g_jobs[i].state == JobState::Writing)
            ++result.queuedWrites;
    for (uint8_t i = 0; i < JOURNAL_SECTORS; ++i)
        result.journalFreeRecords +=
            JOURNAL_RECORDS_PER_SECTOR - g_journalNext[i];
    giveState();
    return result;
}

String describe() {
    const Stats current = stats();
    if (!current.initialized)
        return String("pager=") + statusText(lastStatus());
    String value;
    value.reserve(128);
    value += "pager logical=";
    value += current.logicalUsedBytes;
    value += "/";
    value += current.logicalCapacityBytes;
    value += " sectors e=";
    value += current.erasedSectors;
    value += " l=";
    value += current.liveSectors;
    value += " p=";
    value += current.pendingSectors;
    value += " s=";
    value += current.staleSectors;
    value += " journal=";
    value += current.journalFreeRecords;
    return value;
}

size_t purge(StorageClass storageClass, size_t targetBytes) {
    if (!g_initialized) return 0;
    size_t reclaimed = 0;
    while (targetBytes == 0 || reclaimed < targetBytes) {
        int selected = -1;
        uint32_t oldest = 0;
        size_t selectedBytes = 0;
        takeState();
        if (g_transaction.active) {
            giveState();
            setStatus(Status::Busy);
            break;
        }
        for (size_t i = 0; i < MAX_OBJECTS; ++i) {
            const ObjectState& object = g_objects[i];
            if (!object.used || object.deleted || object.openCount ||
                classFromFlags(object.flags) != storageClass)
                continue;
            if (selected < 0 || newer(oldest, object.lastAccessMs)) {
                selected = static_cast<int>(i);
                oldest = object.lastAccessMs;
                selectedBytes = object.logicalSize;
            }
        }
        giveState();
        if (selected < 0 || !deleteSlot(selected,
                                        DEFAULT_OPERATION_TIMEOUT_MS))
            break;
        reclaimed += selectedBytes;
    }
    return reclaimed;
}

bool enterCritical(uint32_t timeoutMs) {
    if (!g_initialized || !takeState()) {
        setStatus(Status::NotInitialized);
        return false;
    }
    ++g_criticalDepth;
    giveState();
    const uint32_t started = millis();
    while (true) {
        takeState();
        const bool busy = g_flashBusy;
        giveState();
        if (!busy) {
            setStatus(Status::Ok);
            return true;
        }
        if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
            leaveCritical();
            setStatus(Status::Timeout);
            return false;
        }
        delay(1);
    }
}

void leaveCritical() {
    if (!g_initialized || !takeState()) return;
    if (g_criticalDepth) --g_criticalDepth;
    giveState();
    notifyWorker();
}

CriticalGuard::CriticalGuard(uint32_t timeoutMs) {
    if (initialized()) _held = enterCritical(timeoutMs);
}

CriticalGuard::~CriticalGuard() {
    if (_held) leaveCritical();
}

Object::~Object() { close(false); }

Object::Object(Object&& other) noexcept {
    _slot = other._slot;
    _token = other._token;
    other._slot = -1;
    other._token = 0;
}

Object& Object::operator=(Object&& other) noexcept {
    if (this != &other) {
        close(false);
        _slot = other._slot;
        _token = other._token;
        other._slot = -1;
        other._token = 0;
    }
    return *this;
}

bool Object::open(const char* nameSpace, const char* key,
                  StorageClass requestedClass, size_t quotaBytes) {
    close(false);
    if (!nameSpace || !key || !*key) {
        setStatus(Status::InvalidArgument);
        return false;
    }
    if (!begin()) return false;
    const size_t maximum = size_t(g_dataSectorCount) * PAGE_PAYLOAD_SIZE;
    if (!quotaBytes || quotaBytes > maximum) quotaBytes = maximum;
    uint64_t hash;
    uint32_t tag;
    calculateKey(nameSpace, key, hash, tag);

    int slot;
    ObjectState next;
    bool needsRecord = false;
    takeState();
    slot = findObject(hash, tag);
    if (slot < 0) {
        slot = findFreeObject();
        if (slot < 0) {
            giveState();
            setStatus(Status::ObjectLimit);
            return false;
        }
        g_objects[slot].reserved = true;
        next = {};
        next.used = true;
        next.storeHash = hash;
        next.keyTag = tag;
        next.activeEpoch = 0;
        next.logicalSize = 0;
        next.quota = static_cast<uint32_t>(quotaBytes);
        next.flags = makeFlags(requestedClass, false);
        next.sequence = ++g_controlSequence;
        next.transactionId = ++g_transactionSequence;
        needsRecord = true;
    } else {
        next = g_objects[slot];
        const uint16_t requestedFlags = makeFlags(requestedClass, false);
        if (next.deleted) {
            next.deleted = false;
            next.logicalSize = 0;
            ++next.activeEpoch;
            next.transactionId = ++g_transactionSequence;
            needsRecord = true;
        }
        if ((next.flags & CONTROL_CLASS_MASK) !=
                (requestedFlags & CONTROL_CLASS_MASK) ||
            next.quota != quotaBytes) {
            next.flags = requestedFlags;
            next.quota = static_cast<uint32_t>(quotaBytes);
            needsRecord = true;
        }
        if (needsRecord) next.sequence = ++g_controlSequence;
    }
    giveState();

    if (needsRecord &&
        !submitControl(recordFromState(next), DEFAULT_OPERATION_TIMEOUT_MS)) {
        takeState();
        if (!g_objects[slot].used) g_objects[slot].reserved = false;
        giveState();
        return false;
    }
    takeState();
    if (needsRecord) g_objects[slot] = next;
    g_objects[slot].reserved = false;
    ++g_objects[slot].openCount;
    g_objects[slot].lastAccessMs = millis();
    _slot = slot;
    _token = g_nextHandleToken++;
    if (!_token) _token = g_nextHandleToken++;
    giveState();
    setStatus(Status::Ok);
    return true;
}

void Object::close(bool commitPending) {
    if (_slot < 0) return;
    if (inTransaction()) {
        if (commitPending)
            commit();
        else
            rollback();
    }
    if (takeState()) {
        if (_slot >= 0 && _slot < static_cast<int16_t>(MAX_OBJECTS) &&
            g_objects[_slot].openCount)
            --g_objects[_slot].openCount;
        giveState();
    }
    _slot = -1;
    _token = 0;
}

bool Object::beginTransaction() {
    if (_slot < 0 || !g_initialized) {
        setStatus(Status::NotInitialized);
        return false;
    }
    ObjectState object;
    uint32_t candidate;
    takeState();
    if (g_transaction.active || !g_objects[_slot].used ||
        g_objects[_slot].deleted ||
        g_objects[_slot].activeEpoch == 0xFFFFFFFFU) {
        giveState();
        setStatus(Status::Busy);
        return false;
    }
    object = g_objects[_slot];
    candidate = object.activeEpoch + 1U;
    giveState();

    // A rolled-back epoch is reused only after every previously committed
    // sector carrying that candidate epoch has been erased. This makes boot
    // recovery exact without retaining an unbounded transaction-id history.
    if (!eraseAbandonedEpoch(object.storeHash, object.keyTag, candidate,
                             DEFAULT_OPERATION_TIMEOUT_MS))
        return false;

    takeState();
    if (g_transaction.active ||
        g_objects[_slot].activeEpoch + 1U != candidate) {
        giveState();
        setStatus(Status::Busy);
        return false;
    }
    clearTransactionLocked();
    g_transaction.active = true;
    g_transaction.objectSlot = _slot;
    g_transaction.ownerToken = _token;
    g_transaction.candidateEpoch = candidate;
    g_transaction.transactionId = ++g_transactionSequence;
    g_transaction.logicalSize = g_objects[_slot].logicalSize;
    giveState();
    setStatus(Status::Ok);
    return true;
}

bool Object::inTransaction() const {
    if (_slot < 0 || !g_initialized || !takeState()) return false;
    const bool active = g_transaction.active &&
                        g_transaction.objectSlot == _slot &&
                        g_transaction.ownerToken == _token;
    giveState();
    return active;
}

size_t Object::read(size_t offset, void* destination, size_t length) {
    if (_slot < 0 || (!destination && length)) {
        setStatus(Status::InvalidArgument);
        return 0;
    }
    if (!length) return 0;
    uint64_t hash;
    uint32_t tag;
    size_t logicalSize;
    uint32_t transactionId = 0;
    bool ownTransaction = false;
    takeState();
    hash = g_objects[_slot].storeHash;
    tag = g_objects[_slot].keyTag;
    ownTransaction = g_transaction.active &&
                     g_transaction.objectSlot == _slot &&
                     g_transaction.ownerToken == _token;
    logicalSize = ownTransaction ? g_transaction.logicalSize
                                 : g_objects[_slot].logicalSize;
    if (ownTransaction) transactionId = g_transaction.transactionId;
    g_objects[_slot].lastAccessMs = millis();
    giveState();
    if (ownTransaction &&
        !waitForJobs(_token, transactionId, DEFAULT_OPERATION_TIMEOUT_MS))
        return 0;
    if (offset >= logicalSize) return 0;
    if (length > logicalSize - offset) length = logicalSize - offset;

    uint8_t* output = static_cast<uint8_t*>(destination);
    size_t completed = 0;
    while (completed < length) {
        const size_t absolute = offset + completed;
        const uint16_t page = absolute / PAGE_PAYLOAD_SIZE;
        const size_t inside = absolute % PAGE_PAYLOAD_SIZE;
        size_t amount = PAGE_PAYLOAD_SIZE - inside;
        if (amount > length - completed) amount = length - completed;
        uint16_t sector;
        if (!pinReadSector(hash, tag, page, _token, sector)) {
            memset(output + completed, 0, amount);
        } else {
            DataHeader header;
            if (!readDataHeader(sector, header) || !validHeaderOnly(header)) {
                unpinSector(sector);
                setStatus(Status::IoError);
                return completed;
            }
            size_t stored = 0;
            if (inside < header.payloadLength) {
                stored = header.payloadLength - inside;
                if (stored > amount) stored = amount;
                if (!flashRead(dataOffset(sector) + PAGE_HEADER_SIZE + inside,
                               output + completed, stored)) {
                    unpinSector(sector);
                    setStatus(Status::IoError);
                    return completed;
                }
            }
            if (stored < amount)
                memset(output + completed + stored, 0, amount - stored);
            unpinSector(sector);
        }
        completed += amount;
    }
    setStatus(Status::Ok);
    return completed;
}

size_t Object::write(size_t offset, const void* source, size_t length,
                     bool nonBlocking) {
    if (_slot < 0 || (!source && length)) {
        setStatus(Status::InvalidArgument);
        return 0;
    }
    if (!length) return 0;
    if (!inTransaction() && !beginTransaction()) return 0;
    size_t quota;
    takeState();
    quota = g_objects[_slot].quota;
    giveState();
    if (offset > quota || length > quota - offset) {
        setStatus(Status::QuotaExceeded);
        return 0;
    }

    const uint8_t* input = static_cast<const uint8_t*>(source);
    size_t completed = 0;
    while (completed < length) {
        const size_t absolute = offset + completed;
        const uint16_t page = absolute / PAGE_PAYLOAD_SIZE;
        const size_t inside = absolute % PAGE_PAYLOAD_SIZE;
        size_t amount = PAGE_PAYLOAD_SIZE - inside;
        if (amount > length - completed) amount = length - completed;
        size_t oldSize;
        takeState();
        oldSize = g_transaction.logicalSize;
        const size_t end = absolute + amount;
        if (end > g_transaction.logicalSize) g_transaction.logicalSize = end;
        giveState();
        if (!queuePageWrite(_slot, _token, page, inside, input + completed,
                            amount, nonBlocking)) {
            takeState();
            if (g_transaction.logicalSize == absolute + amount)
                g_transaction.logicalSize = oldSize;
            giveState();
            return completed;
        }
        completed += amount;
    }
    return completed;
}

size_t Object::append(const void* source, size_t length, bool nonBlocking) {
    return write(size(), source, length, nonBlocking);
}

bool Object::truncate(size_t length) {
    if (_slot < 0) {
        setStatus(Status::InvalidArgument);
        return false;
    }
    if (!inTransaction() && !beginTransaction()) return false;
    takeState();
    const size_t quota = g_objects[_slot].quota;
    if (length > quota) {
        giveState();
        setStatus(Status::QuotaExceeded);
        return false;
    }
    g_transaction.logicalSize = length;
    giveState();
    setStatus(Status::Ok);
    return true;
}

bool Object::visit(size_t offset, size_t length, VisitCallback callback,
                   void* context) {
    if (_slot < 0 || !callback) {
        setStatus(Status::InvalidArgument);
        return false;
    }
    const size_t logicalSize = size();
    if (offset >= logicalSize || !length) return true;
    if (length > logicalSize - offset) length = logicalSize - offset;
    if (inTransaction() && !flush()) return false;

    uint64_t hash;
    uint32_t tag;
    takeState();
    hash = g_objects[_slot].storeHash;
    tag = g_objects[_slot].keyTag;
    giveState();
    static const uint8_t zeros[64] = {};
    size_t completed = 0;
    while (completed < length) {
        const size_t absolute = offset + completed;
        const uint16_t page = absolute / PAGE_PAYLOAD_SIZE;
        const size_t inside = absolute % PAGE_PAYLOAD_SIZE;
        size_t amount = PAGE_PAYLOAD_SIZE - inside;
        if (amount > length - completed) amount = length - completed;
        uint16_t sector;
        if (!pinReadSector(hash, tag, page, _token, sector)) {
            size_t zeroed = 0;
            while (zeroed < amount) {
                size_t part = amount - zeroed;
                if (part > sizeof(zeros)) part = sizeof(zeros);
                if (!callback(zeros, part, absolute + zeroed, context)) {
                    setStatus(Status::CallbackAborted);
                    return false;
                }
                zeroed += part;
            }
        } else {
            DataHeader header;
            if (!readDataHeader(sector, header) || !validHeaderOnly(header)) {
                unpinSector(sector);
                setStatus(Status::IoError);
                return false;
            }
            size_t stored = inside < header.payloadLength
                                ? header.payloadLength - inside
                                : 0;
            if (stored > amount) stored = amount;
            bool ok = true;
            if (stored) {
                ok = flashVisit(dataOffset(sector) + PAGE_HEADER_SIZE + inside,
                                stored, absolute, callback, context);
            }
            unpinSector(sector);
            if (!ok) {
                setStatus(Status::CallbackAborted);
                return false;
            }
            size_t zeroed = stored;
            while (zeroed < amount) {
                size_t part = amount - zeroed;
                if (part > sizeof(zeros)) part = sizeof(zeros);
                if (!callback(zeros, part, absolute + zeroed, context)) {
                    setStatus(Status::CallbackAborted);
                    return false;
                }
                zeroed += part;
            }
        }
        completed += amount;
    }
    setStatus(Status::Ok);
    return true;
}

bool Object::flushAsync() {
    if (!inTransaction()) return true;
    notifyWorker();
    setStatus(Status::Ok);
    return true;
}

bool Object::flush(uint32_t timeoutMs) {
    if (!inTransaction()) return true;
    uint32_t transactionId;
    takeState();
    transactionId = g_transaction.transactionId;
    giveState();
    return waitForJobs(_token, transactionId, timeoutMs);
}

bool Object::commit(uint32_t timeoutMs) {
    if (!inTransaction()) return true;
    if (!flush(timeoutMs)) return false;

    ObjectState next;
    uint32_t transactionId;
    takeState();
    if (g_transaction.failed) {
        giveState();
        setStatus(Status::TransactionFailed);
        return false;
    }
    next = g_objects[_slot];
    next.activeEpoch = g_transaction.candidateEpoch;
    next.logicalSize = static_cast<uint32_t>(g_transaction.logicalSize);
    next.transactionId = g_transaction.transactionId;
    next.sequence = ++g_controlSequence;
    next.deleted = false;
    next.flags &= ~CONTROL_FLAG_DELETED;
    transactionId = g_transaction.transactionId;
    giveState();

    if (!submitControl(recordFromState(next), timeoutMs)) return false;

    takeState();
    if (!g_transaction.active ||
        g_transaction.transactionId != transactionId ||
        g_transaction.ownerToken != _token) {
        giveState();
        setStatus(Status::Corrupt);
        return false;
    }
    g_objects[_slot] = next;
    const size_t pageCount =
        (next.logicalSize + PAGE_PAYLOAD_SIZE - 1U) / PAGE_PAYLOAD_SIZE;
    for (uint16_t page = 0; page < g_dataSectorCount; ++page) {
        const uint16_t value = g_transaction.pendingByPage[page];
        if (!value) continue;
        const uint16_t sector = value - 1U;
        if (page < pageCount) {
            uint16_t previous = INDEX_TOMBSTONE;
            if (!indexSet(next.storeHash, next.keyTag, page, sector,
                          &previous)) {
                rebuildIndex();
                indexSet(next.storeHash, next.keyTag, page, sector,
                         &previous);
            }
            if (previous != INDEX_TOMBSTONE && previous != sector &&
                g_sectorState[previous] == SectorState::Live)
                g_sectorState[previous] = SectorState::Stale;
            g_sectorState[sector] = SectorState::Live;
        } else {
            g_sectorState[sector] = SectorState::Stale;
        }
    }
    removeObjectPages(next.storeHash, next.keyTag, pageCount);
    clearTransactionLocked();
    giveState();
    notifyWorker();
    setStatus(Status::Ok);
    return true;
}

bool Object::rollback(uint32_t timeoutMs) {
    if (!inTransaction()) return true;
    uint32_t transactionId;
    takeState();
    transactionId = g_transaction.transactionId;
    giveState();
    if (!waitForJobs(_token, transactionId, timeoutMs)) {
        // A completed I/O failure is still safe to roll back; an outstanding
        // timeout is not, because the worker owns a buffer.
        if (lastStatus() == Status::Timeout) return false;
    }
    takeState();
    if (g_transaction.active && g_transaction.ownerToken == _token &&
        g_transaction.transactionId == transactionId) {
        for (uint16_t page = 0; page < g_dataSectorCount; ++page) {
            const uint16_t value = g_transaction.pendingByPage[page];
            if (value) g_sectorState[value - 1U] = SectorState::Stale;
        }
        clearTransactionLocked();
    }
    giveState();
    notifyWorker();
    setStatus(Status::Ok);
    return true;
}

bool Object::erase(uint32_t timeoutMs) {
    if (_slot < 0) {
        setStatus(Status::InvalidArgument);
        return false;
    }
    if (inTransaction() && !rollback(timeoutMs)) return false;
    ObjectState next;
    takeState();
    next = g_objects[_slot];
    next.deleted = true;
    next.flags |= CONTROL_FLAG_DELETED;
    next.logicalSize = 0;
    ++next.activeEpoch;
    next.transactionId = ++g_transactionSequence;
    next.sequence = ++g_controlSequence;
    giveState();
    if (!submitControl(recordFromState(next), timeoutMs)) return false;
    takeState();
    g_objects[_slot] = next;
    removeObjectPages(next.storeHash, next.keyTag, 0);
    giveState();
    notifyWorker();
    setStatus(Status::Ok);
    return true;
}

size_t Object::size() const {
    if (_slot < 0 || !g_initialized || !takeState()) return 0;
    const size_t value =
        g_transaction.active && g_transaction.objectSlot == _slot &&
                g_transaction.ownerToken == _token
            ? g_transaction.logicalSize
            : g_objects[_slot].logicalSize;
    giveState();
    return value;
}

size_t Object::capacity() const {
    if (_slot < 0 || !g_initialized || !takeState()) return 0;
    const size_t value = g_objects[_slot].quota;
    giveState();
    return value;
}

StorageClass Object::storageClass() const {
    if (_slot < 0 || !g_initialized || !takeState())
        return StorageClass::Volatile;
    const StorageClass value = classFromFlags(g_objects[_slot].flags);
    giveState();
    return value;
}

PagedStream::~PagedStream() { close(false); }

bool PagedStream::open(const char* nameSpace, const char* key,
                       StorageClass storageClass, size_t quotaBytes,
                       bool truncateExisting, bool writeable) {
    close(false);
    if (!_object.open(nameSpace, key, storageClass, quotaBytes)) return false;
    _position = 0;
    _peeked = -1;
    _writeable = writeable;
    if (truncateExisting) {
        if (!writeable || !_object.beginTransaction() ||
            !_object.truncate(0)) {
            close(false);
            return false;
        }
    }
    return true;
}

bool PagedStream::close(bool commitPending) {
    bool ok = true;
    if (_object.isOpen() && _object.inTransaction())
        ok = commitPending ? _object.commit() : _object.rollback();
    _object.close(false);
    _position = 0;
    _peeked = -1;
    _writeable = false;
    return ok;
}

bool PagedStream::seek(size_t position) {
    if (!_object.isOpen() || position > _object.capacity()) return false;
    _position = position;
    _peeked = -1;
    return true;
}

int PagedStream::available() {
    if (!_object.isOpen()) return 0;
    const size_t objectSize = _object.size();
    const size_t remaining = objectSize > _position ? objectSize - _position : 0;
    return remaining > 0x7FFFFFFFU ? 0x7FFFFFFF
                                   : static_cast<int>(remaining);
}

int PagedStream::read() {
    if (_peeked >= 0) {
        const int value = _peeked;
        _peeked = -1;
        ++_position;
        return value;
    }
    uint8_t value;
    if (_object.read(_position, &value, 1) != 1) return -1;
    ++_position;
    return value;
}

int PagedStream::peek() {
    if (_peeked >= 0) return _peeked;
    uint8_t value;
    if (_object.read(_position, &value, 1) != 1) return -1;
    _peeked = value;
    return _peeked;
}

void PagedStream::flush() {
    if (_object.isOpen() && _object.inTransaction()) _object.commit();
}

size_t PagedStream::write(uint8_t value) { return write(&value, 1); }

size_t PagedStream::write(const uint8_t* buffer, size_t length) {
    if (!_writeable || !_object.isOpen() || (!buffer && length)) return 0;
    _peeked = -1;
    const size_t written = _object.write(_position, buffer, length);
    _position += written;
    return written;
}

}  // namespace FlashPager
