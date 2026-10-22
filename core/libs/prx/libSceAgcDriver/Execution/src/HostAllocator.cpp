#ifdef _WIN32

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <malloc.h>
#include <new>
#include <windows.h>

namespace {

constexpr std::size_t Granule = 16;
constexpr std::size_t LargestCached = 1024;
constexpr std::size_t Classes = LargestCached / Granule;

constexpr std::uint16_t Depth(std::size_t sizeClass) {
    return sizeClass < 256 / Granule ? 64 : 16;
}

struct Cache {
    void* heads[Classes];
    std::uint16_t counts[Classes];
    std::uint64_t hits;
    std::uint64_t misses;
    std::uint64_t large;
    std::uint64_t cachedFrees;
    std::uint64_t freed;
    std::uint64_t batchesIn;
    std::uint64_t batchesOut;
    std::uint64_t operations;
};

struct Batch {
    Batch* next;
    std::uint16_t count;
};

constexpr std::size_t MaxBatches = 256;

struct Transfer {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    Batch* batches = nullptr;
    std::size_t count = 0;
};

Transfer transfers[Classes];

bool Switch(const char* name, std::atomic<int>& state) noexcept {
    auto value = state.load(std::memory_order_relaxed);
    if (value == 0) {
        const DWORD error = GetLastError();
        char buffer[8];
        value = GetEnvironmentVariableA(name, buffer, sizeof(buffer)) != 0 ? 2 : 1;
        SetLastError(error);
        state.store(value, std::memory_order_relaxed);
    }
    return value == 2;
}

bool TransferEnabled() noexcept {
    static std::atomic<int> state{0};
    auto value = state.load(std::memory_order_relaxed);
    if (value == 0) {
        const DWORD error = GetLastError();
        char buffer[8] = {};
        const DWORD length = GetEnvironmentVariableA("APS5_ALLOC_TRANSFER", buffer, sizeof(buffer));
        value = length == 1 && buffer[0] == '0' ? 1 : 2;
        SetLastError(error);
        state.store(value, std::memory_order_relaxed);
    }
    return value == 2;
}

bool StatsEnabled() noexcept {
    static std::atomic<int> state{0};
    return Switch("APS5_ALLOC_STATS", state);
}

void Lock(Transfer& transfer) noexcept {
    while (transfer.busy.test_and_set(std::memory_order_acquire)) _mm_pause();
}

void Unlock(Transfer& transfer) noexcept {
    transfer.busy.clear(std::memory_order_release);
}

bool PushBatch(std::size_t sizeClass, void* head, std::uint16_t count) noexcept {
    auto& transfer = transfers[sizeClass];
    auto* batch = static_cast<Batch*>(head);
    *(reinterpret_cast<void**>(static_cast<char*>(head) + sizeof(Batch))) = *static_cast<void**>(head);
    batch->count = count;
    Lock(transfer);
    if (transfer.count >= MaxBatches) {
        Unlock(transfer);
        return false;
    }
    batch->next = transfer.batches;
    transfer.batches = batch;
    ++transfer.count;
    Unlock(transfer);
    return true;
}

void* PopBatch(std::size_t sizeClass, std::uint16_t& count) noexcept {
    auto& transfer = transfers[sizeClass];
    Lock(transfer);
    auto* batch = transfer.batches;
    if (batch != nullptr) {
        transfer.batches = batch->next;
        --transfer.count;
    }
    Unlock(transfer);
    if (batch == nullptr) return nullptr;
    count = batch->count;
    void* head = batch;
    *static_cast<void**>(head) = *(reinterpret_cast<void**>(static_cast<char*>(head) + sizeof(Batch)));
    return head;
}

void Report(Cache* cache) noexcept {
    if ((++cache->operations & ((std::uint64_t{1} << 21) - 1)) != 0 || !StatsEnabled()) return;
    std::fprintf(stderr, "[alloc] thread %lu: %llu cache hits, %llu misses to malloc, %llu above %zu bytes, %llu frees kept, %llu freed, batches %llu taken / %llu given\n", GetCurrentThreadId(), static_cast<unsigned long long>(cache->hits), static_cast<unsigned long long>(cache->misses), static_cast<unsigned long long>(cache->large), LargestCached, static_cast<unsigned long long>(cache->cachedFrees), static_cast<unsigned long long>(cache->freed), static_cast<unsigned long long>(cache->batchesIn), static_cast<unsigned long long>(cache->batchesOut));
    cache->hits = cache->misses = cache->large = cache->cachedFrees = cache->freed = cache->batchesIn = cache->batchesOut = 0;
}

Cache* const Closed = reinterpret_cast<Cache*>(std::uintptr_t{1});

constexpr unsigned long TlsSlotsOffset = 0x1480;
constexpr DWORD DirectSlots = 64;

constexpr DWORD Unset = TLS_OUT_OF_INDEXES;
constexpr DWORD Failed = TLS_OUT_OF_INDEXES - 1;
std::atomic<DWORD> tlsSlot{Unset};
std::atomic<DWORD> flsSlot{FLS_OUT_OF_INDEXES};

void Drain(Cache* cache) noexcept {
    for (std::size_t sizeClass = 0; sizeClass < Classes; ++sizeClass) {
        while (void* block = cache->heads[sizeClass]) {
            cache->heads[sizeClass] = *static_cast<void**>(block);
            std::free(block);
        }
        cache->counts[sizeClass] = 0;
    }
}

void WINAPI ThreadEnded(void* data) {
    auto* cache = static_cast<Cache*>(data);
    if (cache == nullptr || cache == Closed) return;
    const auto slot = tlsSlot.load(std::memory_order_acquire);
    if (slot < Failed) TlsSetValue(slot, Closed);
    Drain(cache);
    std::free(cache);
}

DWORD Slot() noexcept {
    auto slot = tlsSlot.load(std::memory_order_acquire);
    if (slot != Unset) return slot;
    const DWORD error = GetLastError();
    const DWORD fls = FlsAlloc(ThreadEnded);
    DWORD tls = fls == FLS_OUT_OF_INDEXES ? TLS_OUT_OF_INDEXES : TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES || tls >= Failed) {
        if (tls != TLS_OUT_OF_INDEXES) TlsFree(tls);
        if (fls != FLS_OUT_OF_INDEXES) FlsFree(fls);
        tls = Failed;
    }
    DWORD expected = Unset;
    if (tlsSlot.compare_exchange_strong(expected, tls, std::memory_order_acq_rel)) {
        if (tls != Failed) flsSlot.store(fls, std::memory_order_release);
        SetLastError(error);
        return tls;
    }
    if (tls != Failed) {
        TlsFree(tls);
        FlsFree(fls);
    }
    SetLastError(error);
    return expected;
}

Cache* SlotValue(DWORD slot) noexcept {
    if (slot < DirectSlots) return reinterpret_cast<Cache*>(__readgsqword(TlsSlotsOffset + slot * sizeof(void*)));
    const DWORD error = GetLastError();
    auto* value = static_cast<Cache*>(TlsGetValue(slot));
    SetLastError(error);
    return value;
}

Cache* CreateCache(DWORD slot) noexcept {
    const auto fls = flsSlot.load(std::memory_order_acquire);
    if (fls == FLS_OUT_OF_INDEXES) return nullptr;
    auto* cache = static_cast<Cache*>(std::calloc(1, sizeof(Cache)));
    if (cache == nullptr) return nullptr;
    const DWORD error = GetLastError();
    if (!FlsSetValue(fls, cache) || !TlsSetValue(slot, cache)) {
        FlsSetValue(fls, nullptr);
        std::free(cache);
        cache = nullptr;
    }
    SetLastError(error);
    return cache;
}

Cache* CurrentCache() noexcept {
    const auto slot = Slot();
    if (slot == Failed) return nullptr;
    auto* cache = SlotValue(slot);
    if (cache == Closed) return nullptr;
    return cache != nullptr ? cache : CreateCache(slot);
}

void* TryAllocate(std::size_t bytes) noexcept {
    if (bytes == 0) bytes = 1;
    if (bytes <= LargestCached) {
        const auto sizeClass = (bytes - 1) / Granule;
        if (auto* cache = CurrentCache()) {
            Report(cache);
            if (cache->heads[sizeClass] == nullptr && TransferEnabled()) {
                std::uint16_t count = 0;
                if (void* head = PopBatch(sizeClass, count)) {
                    cache->heads[sizeClass] = head;
                    cache->counts[sizeClass] = count;
                    ++cache->batchesIn;
                }
            }
            if (void* block = cache->heads[sizeClass]) {
                cache->heads[sizeClass] = *static_cast<void**>(block);
                --cache->counts[sizeClass];
                ++cache->hits;
                return block;
            }
            ++cache->misses;
        }
        return std::malloc((sizeClass + 1) * Granule);
    }
    if (auto* cache = CurrentCache()) {
        Report(cache);
        ++cache->large;
    }
    return std::malloc(bytes);
}

void* Allocate(std::size_t bytes) {
    for (;;) {
        if (void* block = TryAllocate(bytes)) return block;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void Release(void* block, std::size_t bytes = 0) noexcept {
    if (block == nullptr) return;
    if (auto* cache = CurrentCache()) {
        const auto usable = bytes != 0 && bytes % Granule == 0 ? bytes : _msize(block);
        if (usable >= Granule && usable < LargestCached + Granule) {
            const auto sizeClass = usable / Granule - 1;
            if (cache->counts[sizeClass] >= Depth(sizeClass) && TransferEnabled() && usable >= sizeof(Batch) + sizeof(void*) && PushBatch(sizeClass, cache->heads[sizeClass], cache->counts[sizeClass])) {
                cache->heads[sizeClass] = nullptr;
                cache->counts[sizeClass] = 0;
                ++cache->batchesOut;
            }
            if (cache->counts[sizeClass] < Depth(sizeClass)) {
                *static_cast<void**>(block) = cache->heads[sizeClass];
                cache->heads[sizeClass] = block;
                ++cache->counts[sizeClass];
                ++cache->cachedFrees;
                return;
            }
        }
        ++cache->freed;
    }
    std::free(block);
}

}

void* operator new(std::size_t bytes) { return Allocate(bytes); }
void* operator new[](std::size_t bytes) { return Allocate(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* block) noexcept { Release(block); }
void operator delete[](void* block) noexcept { Release(block); }
void operator delete(void* block, std::size_t bytes) noexcept { Release(block, bytes); }
void operator delete[](void* block, std::size_t bytes) noexcept { Release(block, bytes); }
void operator delete(void* block, const std::nothrow_t&) noexcept { Release(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { Release(block); }

#endif
