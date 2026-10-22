#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <pthread.h>
#include <windows.h>

namespace {

struct EmulatedObject {
    std::size_t size;
    std::size_t align;
    union {
        std::uintptr_t index;
        void* pointer;
    } location;
    void* initial;
};

using GetAddress = void* (*)(EmulatedObject*);

constexpr unsigned long TlsSlotsOffset = 0x1480;
constexpr DWORD DirectSlots = 64;
constexpr std::uintptr_t GrowBy = 32;

enum class Route : int { Unknown, Library, Native };

std::atomic<Route> route{Route::Unknown};
std::atomic<GetAddress> library{nullptr};
std::atomic<std::uintptr_t> imageBegin{0};
std::atomic<std::uintptr_t> imageEnd{0};
std::atomic<std::uintptr_t> nextIndex{1};
DWORD slot = TLS_OUT_OF_INDEXES;
pthread_key_t releaseKey;
SRWLOCK indexLock = SRWLOCK_INIT;

bool NativeRequested() {
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("APS5_NATIVE_TLS", value, sizeof(value));
    return !(length == 1 && value[0] == '0');
}

void ImageRange() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ImageRange), &module))
        return;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    imageBegin.store(base, std::memory_order_relaxed);
    imageEnd.store(base + nt->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
}

void Release(void* data) {
    auto** blocks = static_cast<void**>(data);
    const auto capacity = reinterpret_cast<std::uintptr_t>(blocks[0]);
    for (std::uintptr_t index = 1; index < capacity; ++index)
        if (blocks[index] != nullptr) std::free(static_cast<void**>(blocks[index])[-1]);
    std::free(blocks);
    TlsSetValue(slot, nullptr);
}

Route Decide() {
    const DWORD error = GetLastError();
    HMODULE gcc = GetModuleHandleA("libgcc_s_seh-1.dll");
    const auto found = gcc != nullptr ? reinterpret_cast<GetAddress>(reinterpret_cast<void*>(GetProcAddress(gcc, "__emutls_get_address"))) : nullptr;
    library.store(found, std::memory_order_relaxed);
    Route decided = Route::Library;
    if (found == nullptr || NativeRequested()) {
        AcquireSRWLockExclusive(&indexLock);
        if (slot == TLS_OUT_OF_INDEXES) {
            const DWORD allocated = TlsAlloc();
            if (allocated != TLS_OUT_OF_INDEXES && pthread_key_create(&releaseKey, Release) == 0)
                slot = allocated;
            else if (allocated != TLS_OUT_OF_INDEXES)
                TlsFree(allocated);
        }
        ReleaseSRWLockExclusive(&indexLock);
        ImageRange();
        if (slot != TLS_OUT_OF_INDEXES && imageEnd.load(std::memory_order_relaxed) != 0) decided = Route::Native;
        if (decided == Route::Library && found == nullptr) std::abort();
    }
    SetLastError(error);
    Route expected = Route::Unknown;
    route.compare_exchange_strong(expected, decided, std::memory_order_release, std::memory_order_acquire);
    return expected == Route::Unknown ? decided : expected;
}

std::uintptr_t AssignIndex(EmulatedObject* object) {
    AcquireSRWLockExclusive(&indexLock);
    std::uintptr_t index = __atomic_load_n(&object->location.index, __ATOMIC_RELAXED);
    if (index == 0) {
        index = nextIndex.fetch_add(1, std::memory_order_relaxed);
        __atomic_store_n(&object->location.index, index, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&indexLock);
    return index;
}

void** Blocks() {
    if (slot < DirectSlots) return reinterpret_cast<void**>(__readgsqword(TlsSlotsOffset + slot * sizeof(void*)));
    const DWORD error = GetLastError();
    auto** blocks = static_cast<void**>(TlsGetValue(slot));
    SetLastError(error);
    return blocks;
}

void** Grow(void** blocks, std::uintptr_t index) {
    const auto capacity = blocks != nullptr ? reinterpret_cast<std::uintptr_t>(blocks[0]) : 0;
    const auto grown = std::max<std::uintptr_t>(index + GrowBy, capacity * 2);
    auto** larger = static_cast<void**>(std::calloc(grown, sizeof(void*)));
    if (larger == nullptr) std::abort();
    if (blocks != nullptr) std::memcpy(larger + 1, blocks + 1, (capacity - 1) * sizeof(void*));
    larger[0] = reinterpret_cast<void*>(grown);
    if (!TlsSetValue(slot, larger) || pthread_setspecific(releaseKey, larger) != 0) std::abort();
    std::free(blocks);
    return larger;
}

void* CreateBlock(const EmulatedObject* object) {
    const auto align = object->align > sizeof(void*) ? object->align : sizeof(void*);
    auto* raw = static_cast<std::byte*>(std::malloc(object->size + sizeof(void*) + align - 1));
    if (raw == nullptr) std::abort();
    const auto start = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
    auto* block = reinterpret_cast<void*>(start);
    static_cast<void**>(block)[-1] = raw;
    if (object->initial != nullptr)
        std::memcpy(block, object->initial, object->size);
    else
        std::memset(block, 0, object->size);
    return block;
}

void* NativeAddress(EmulatedObject* object) {
    std::uintptr_t index = __atomic_load_n(&object->location.index, __ATOMIC_ACQUIRE);
    if (index == 0) index = AssignIndex(object);
    void** blocks = Blocks();
    if (blocks != nullptr && index < reinterpret_cast<std::uintptr_t>(blocks[0]) && blocks[index] != nullptr) return blocks[index];
    const DWORD error = GetLastError();
    if (blocks == nullptr || index >= reinterpret_cast<std::uintptr_t>(blocks[0])) blocks = Grow(blocks, index);
    if (blocks[index] == nullptr) blocks[index] = CreateBlock(object);
    SetLastError(error);
    return blocks[index];
}

bool Owned(const EmulatedObject* object) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    return address >= imageBegin.load(std::memory_order_relaxed) && address < imageEnd.load(std::memory_order_relaxed);
}

}

extern "C" void* __emutls_get_address(void* address) {
    auto* object = static_cast<EmulatedObject*>(address);
    Route current = route.load(std::memory_order_acquire);
    if (current == Route::Unknown) current = Decide();
    if (current == Route::Native && Owned(object)) return NativeAddress(object);
    return library.load(std::memory_order_relaxed)(object);
}

#endif
