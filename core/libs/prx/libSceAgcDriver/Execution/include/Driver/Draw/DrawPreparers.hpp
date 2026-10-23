#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPREPARERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPREPARERS_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPreparers {
public:
    using Job = std::function<void(const QueueState&)>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    using Commit = std::function<void()>;
    struct Result {
        Commit commit;
        std::vector<Range> writes;
        std::vector<Range> reads;
        std::vector<Range> preparedReads;
        std::uint64_t preparedFrom = 0;
    };

    static DrawPreparers& Queue0();
    static unsigned Threads();
    static std::size_t Depth();
    static bool OnPreparer();

    void OnIdle(std::function<void()> release);
    void Submit(const QueueState& queue, Job job);
    void Wait(std::uint32_t opcode = 0x100);
    bool Busy() const { return turn.load(std::memory_order_acquire) != submitted.load(std::memory_order_acquire); }
    void AwaitTurn();
    void NoteWrites(const std::vector<Range>& writes);
    bool Finish(Commit commit, std::vector<Range> writes, std::vector<Range> reads, std::vector<Range> preparedReads);
    void Stop();

private:
    struct Item {
        std::uint64_t ticket = 0;
        std::unique_ptr<QueueState> state;
        Job job;
    };
    struct Slot {
        bool done = false;
        std::unique_ptr<Result> result;
        Item item;
    };
    DrawPreparers() = default;
    void start();
    void run();
    void passTurn(std::uint64_t ticket);
    void flush(std::unique_lock<std::mutex>& lock);
    bool flushBatch(std::unique_lock<std::mutex>& lock);
    void noteWrites(std::uint64_t ticket, const std::vector<Range>& writes);
    bool writtenSince(std::span<const Range> reads, std::uint64_t from, std::uint64_t ticket) const;
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable room;
    std::condition_variable turned;
    std::deque<Item> items;
    std::deque<Slot> slots;
    bool flushing = false;
    std::vector<std::unique_ptr<QueueState>> spare;
    std::vector<std::thread> threads;
    std::function<void()> idle;
    std::deque<std::pair<std::uint64_t, std::vector<Range>>> written;
    std::uint64_t writtenFrom = 0;
    std::exception_ptr failure;
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> turn{0};
    std::atomic<bool> failed{false};
    std::uint32_t sleepers = 0;
    std::uint32_t turnWaiters = 0;
    bool roomWaiting = false;
    bool skipping = false;
    bool closing = false;
    std::uint64_t jobs = 0;
    std::uint64_t jobNs = 0;
    std::uint64_t turnNs = 0;
    std::uint64_t copyNs = 0;
    std::uint64_t fullNs = 0;
    std::uint64_t waits = 0;
    std::uint64_t waitNs = 0;
    std::uint64_t depthSum = 0;
    std::uint64_t redone = 0;
    std::uint64_t parked = 0;
    std::uint64_t batches = 0;
    std::uint64_t batched = 0;
    std::array<std::uint64_t, 257> waitOpcodes{};
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

}

#endif
