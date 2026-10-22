#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" void* __emutls_get_address(void*);

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

thread_local int counter = 7;
thread_local std::uint64_t zeroed[512];
struct alignas(64) Aligned {
    int value = 5;
};
thread_local Aligned aligned;

template<int N>
thread_local int numbered = N * 3 + 1;

template<int... N>
bool NumberedIntact(std::integer_sequence<int, N...>) {
    return ((numbered<N> == N * 3 + 1) && ...);
}

template<int... N>
void BumpNumbered(std::integer_sequence<int, N...>) {
    ((numbered<N> += 1000), ...);
}

template<int... N>
bool NumberedBumped(std::integer_sequence<int, N...>) {
    return ((numbered<N> == N * 3 + 1001) && ...);
}

__attribute__((noinline, noipa)) int* CounterAddress() {
    asm volatile("");
    return &counter;
}

bool InImage(const void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module);
    return module == GetModuleHandleW(nullptr);
}

void CheckThread(int seed) {
    constexpr auto sequence = std::make_integer_sequence<int, 80>{};
    Require(counter == 7, "a thread_local does not start from its initializer");
    Require(aligned.value == 5, "an aligned thread_local does not start from its initializer");
    Require(reinterpret_cast<std::uintptr_t>(&aligned) % 64 == 0, "an aligned thread_local is misaligned");
    for (const auto word : zeroed) Require(word == 0, "a zero-initialised thread_local is not zero");
    Require(NumberedIntact(sequence), "many thread_locals do not start from their initializers");
    for (int index = 0; index < 10000; ++index) {
        counter += seed;
        zeroed[index % 512] += 1;
    }
    BumpNumbered(sequence);
    std::this_thread::yield();
    Require(counter == 7 + seed * 10000, "another thread changed this thread's thread_local");
    Require(NumberedBumped(sequence), "many thread_locals lost their values");
}

void CheckThreads() {
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int round = 0; round < 20; ++round) {
        for (int seed = 1; seed <= 8; ++seed)
            threads.emplace_back([seed, &failures] {
                try {
                    CheckThread(seed);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    failures.fetch_add(1);
                }
            });
        for (auto& thread : threads) thread.join();
        threads.clear();
    }
    Require(failures.load() == 0, "a thread saw wrong thread_local values");
}

void CheckLastError() {
    std::thread([] {
        SetLastError(1234);
        counter += 1;
        Require(GetLastError() == 1234, "the first thread_local access changed the last error");
        SetLastError(4321);
        counter += 1;
        Require(GetLastError() == 4321, "a thread_local access changed the last error");
    }).join();
}

void CheckCallOnce() {
    std::once_flag flag;
    std::atomic<int> calls{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) threads.emplace_back([&] { std::call_once(flag, [&] { calls.fetch_add(1); }); });
    for (auto& thread : threads) thread.join();
    Require(calls.load() == 1, "std::call_once ran more or less than once");
}

void ReportSpeed() {
    constexpr int count = 20000000;
    const auto start = std::chrono::steady_clock::now();
    int sum = 0;
    for (int index = 0; index < count; ++index) sum += *CounterAddress();
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::cout << "thread_local access " << elapsed / count << " ns (" << sum % 2 << ")\n";
}

}

int main(int argc, char** argv) {
    const bool native = argc < 2 || std::string(argv[1]) != "library";
    SetEnvironmentVariableA("APS5_NATIVE_TLS", native ? "1" : "0");
    try {
        Require(InImage(reinterpret_cast<const void*>(&__emutls_get_address)), "__emutls_get_address does not resolve to this image");
        CheckThreads();
        CheckLastError();
        CheckCallOnce();
        ReportSpeed();
        std::cout << "native tls tests passed (" << (native ? "native" : "library") << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
