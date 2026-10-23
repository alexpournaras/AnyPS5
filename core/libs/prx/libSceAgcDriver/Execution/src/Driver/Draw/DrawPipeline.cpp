#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPreparers.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission"};

std::atomic<unsigned> runningCommitters{0};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

std::atomic<std::uint64_t> heldReadRanges{0};
std::atomic<std::uint64_t> heldReadQueries{0};
std::atomic<std::uint64_t> heldReadHits{0};

struct CommitParts {
    std::atomic<std::uint64_t> draws{0}, lockNs{0}, beforeNs{0}, drawNs{0}, unindexed{0};
};

CommitParts& commitParts() {
    static CommitParts parts;
    return parts;
}

}

void DrawPipeline::NoteCommitParts(std::uint64_t lockNs, std::uint64_t beforeNs, std::uint64_t drawNs, bool indexed) {
    auto& parts = commitParts();
    parts.draws.fetch_add(1, std::memory_order_relaxed);
    parts.lockNs.fetch_add(lockNs, std::memory_order_relaxed);
    parts.beforeNs.fetch_add(beforeNs, std::memory_order_relaxed);
    parts.drawNs.fetch_add(drawNs, std::memory_order_relaxed);
    if (!indexed) parts.unindexed.fetch_add(1, std::memory_order_relaxed);
}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{64};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{64} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

std::size_t DrawPipeline::QueuedItems() {
    auto& pipeline = Queue0();
    auto lock = pipeline.lockQueue();
    return pipeline.items.size();
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch();
}

bool DrawPipeline::releaseOnEnqueue() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_COMMIT_RELEASE");
        return value != nullptr && std::string(value) == "1";
    }();
    return enabled;
}

std::size_t DrawPipeline::roomBatch() {
    static const std::size_t batch = [] {
        const char* value = std::getenv("APS5_COMMIT_ROOM_BATCH");
        if (value == nullptr) return std::size_t{1};
        return static_cast<std::size_t>(std::clamp<unsigned long long>(std::strtoull(value, nullptr, 10), 1, std::max<std::size_t>(Depth() / 2, 1)));
    }();
    return batch;
}

unsigned DrawPipeline::Committers() {
    return runningCommitters.load(std::memory_order_acquire);
}

void DrawPipeline::awaitPreparedOrder(std::uint32_t opcode) {
    if (DrawPreparers::OnPreparer()) DrawPreparers::Queue0().AwaitTurn();
    else if (Active() && DrawPreparers::Queue0().Busy()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("waiting for the draw preparers under the GPU lock (opcode " + std::to_string(opcode) + ")");
        DrawPreparers::Queue0().Wait(opcode);
    }
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    awaitPreparedOrder();
    if (DrawPreparers::OnPreparer()) DrawPreparers::Queue0().NoteWrites(writes);
    std::vector<Item> dead;
    auto lock = lockQueue();
    rethrowFailure();
    if (closing) {
        lock.unlock();
        commit();
        return;
    }
    place(lock, {std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads)});
    if (released.size() >= 16) dead.swap(released);
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::EnqueueBatch(std::vector<Handed>& batch) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    awaitPreparedOrder();
    std::vector<Item> dead;
    auto lock = lockQueue();
    rethrowFailure();
    for (auto& handed : batch) {
        if (closing) {
            lock.unlock();
            handed.commit();
            lock = lockQueue();
            continue;
        }
        place(lock, {std::move(handed.commit), std::move(handed.writes), 0, {}, std::move(handed.reads)});
    }
    if (released.size() >= 16) dead.swap(released);
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::place(std::unique_lock<std::mutex>& lock, Item item) {
    if (items.size() >= Depth()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline full while the enqueuing thread holds the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        ++idleWaiters;
        idle.wait(lock, [&] { return roomFor(); });
        --idleWaiters;
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    heldReadRanges.fetch_add(item.reads.size(), std::memory_order_relaxed);
    addWrites(item.writes);
    items.push_back(std::move(item));
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) {
        thread = std::thread([this] { run(); });
        runningCommitters.fetch_add(1, std::memory_order_acq_rel);
    }
    if (committerWaiting) wake.notify_one();
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    char reads[64];
    std::snprintf(reads, sizeof(reads), " %llu read ranges, %llu stores held", static_cast<unsigned long long>(heldReadRanges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(heldReadHits.exchange(0, std::memory_order_relaxed)));
    drainText += reads;
    std::fprintf(stderr, "[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room;%s; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, reads, drainText.c_str());
    auto& parts = commitParts();
    const auto partDraws = parts.draws.exchange(0, std::memory_order_relaxed);
    const auto perPart = [&](std::atomic<std::uint64_t>& ns) { return partDraws == 0 ? 0.0 : static_cast<double>(ns.exchange(0, std::memory_order_relaxed)) / 1e3 / static_cast<double>(partDraws); };
    const auto perCommitNs = [&](std::uint64_t ns) { return commits == 0 ? 0.0 : static_cast<double>(ns) / 1e3 / static_cast<double>(commits); };
    const auto lockUs = perPart(parts.lockNs);
    const auto beforeUs = perPart(parts.beforeNs);
    const auto drawUs = perPart(parts.drawNs);
    const auto unindexed = parts.unindexed.exchange(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[draw] committer split (10 s): %llu draws: GPU lock wait %.2f us, labels and writers %.2f us, Graphics::Draw %.2f us; per commit (%.2f us): outside the commit: locking the queue %.2f us, releasing the draw %.2f us, queue bookkeeping %.2f us, waiting for and taking the next draw %.2f us; draws without an index buffer %llu (%.1f%%)\n", static_cast<unsigned long long>(partDraws), lockUs, beforeUs, drawUs, perCommit, perCommitNs(relockNs), perCommitNs(releaseNs), perCommitNs(bookkeepNs), perCommitNs(idleNs), static_cast<unsigned long long>(unindexed), partDraws == 0 ? 0.0 : 100.0 * static_cast<double>(unindexed) / static_cast<double>(partDraws));
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = idleNs = relockNs = releaseNs = bookkeepNs = 0;
    drains = {};
    drainWaitNs = {};
}

void DrawPipeline::run() {
    PerformanceTrace::BindThread("draw committer");
    static const bool raised = std::getenv("APS5_RAISE_COMMITTER") != nullptr && std::string(std::getenv("APS5_RAISE_COMMITTER")) != "0";
    if (raised) {
        try {
            RaiseWorkerThreadPriority("draw committer");
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[draw] committer priority not raised: %s\n", error.what());
        }
    }
    try {
        PinDrawThread("draw committer");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[draw] committer not pinned: %s\n", error.what());
    }
    StartCommitterSampler();
    static const bool oneLock = [] {
        const char* value = std::getenv("APS5_COMMIT_ONE_LOCK");
        return value != nullptr && std::string(value) == "1";
    }();
    auto idleSince = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock;
    for (;;) {
        Commit* commit = nullptr;
        if (!lock.owns_lock() || items.empty()) {
            if (lock.owns_lock()) lock.unlock();
            SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
            lock = lockQueue();
        }
        committerWaiting = true;
        wake.wait(lock, [&] { return !items.empty() || closing; });
        committerWaiting = false;
        if (items.empty()) break;
        commit = stopped ? nullptr : &items.front().commit;
        lock.unlock();
        const auto start = std::chrono::steady_clock::now();
        const auto idled = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - idleSince).count());
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
        try {
            if (commit != nullptr) (*commit)();
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        const auto committed = std::chrono::steady_clock::now();
        const auto spent = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(committed - start).count());
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        lock = lockQueue();
        const auto locked = std::chrono::steady_clock::now();
        relockNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(locked - committed).count());
        removeWrites(items.front().writes);
        const auto releasing = std::chrono::steady_clock::now();
        if (releaseOnEnqueue()) {
            if (released.capacity() == 0) released.reserve(32);
            released.push_back(std::move(items.front()));
        }
        items.pop_front();
        idleSince = std::chrono::steady_clock::now();
        const auto releaseSpan = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(idleSince - releasing).count());
        releaseNs += releaseSpan;
        bookkeepNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(idleSince - locked).count()) - releaseSpan;
        idleNs += idled;
        ++commits;
        commitNs += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        if (idleWaiters != 0 && (items.empty() || roomFor())) idle.notify_all();
        if (!oneLock) lock.unlock();
    }
    if (lock.owns_lock()) lock.unlock();
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

void DrawPipeline::Stop() {
    std::thread committer;
    {
        auto lock = lockQueue();
        closing = true;
        committer = std::move(thread);
    }
    wake.notify_all();
    if (!committer.joinable()) return;
    if (committer.get_id() == std::this_thread::get_id()) committer.detach();
    else committer.join();
    std::vector<Item> dead;
    auto lock = lockQueue();
    dead.swap(released);
    lock.unlock();
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    awaitPreparedOrder(opcode);
    if (!Busy()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    const auto start = std::chrono::steady_clock::now();
    SpinUntil(HandoffSpinWindow() * 2, [&] { return !Busy(); });
    std::vector<Item> dead;
    auto lock = lockQueue();
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
    --idleWaiters;
    dead.swap(released);
    const auto index = static_cast<std::size_t>(reason);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += elapsedNs(start);
    rethrowFailure();
}

std::uint64_t DrawPipeline::DrainRequestsByOpcode(std::uint32_t opcode) {
    return Queue0().drainRequests[std::min<std::uint32_t>(opcode, 0x100)].load(std::memory_order_relaxed);
}

std::unique_lock<std::mutex> DrawPipeline::lockQueue() {
    LockSpinning(mutex, HandoffSpinWindow());
    return std::unique_lock<std::mutex>(mutex, std::adopt_lock);
}

bool DrawPipeline::HeldReads(std::uint64_t address, std::size_t bytes) {
    static const bool unordered = std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") != nullptr;
    if (bytes == 0 || unordered) return false;
    const auto end = address + bytes;
    auto& pipeline = Queue0();
    auto lock = pipeline.lockQueue();
    heldReadQueries.fetch_add(1, std::memory_order_relaxed);
    for (const auto& item : pipeline.items) {
        for (const auto& [begin, limit] : item.reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void DrawPipeline::ReleaseRunningReads() {
    auto lock = lockQueue();
    if (!items.empty()) items.front().reads.clear();
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    const auto end = address + bytes;
    auto lock = lockQueue();
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
    }
    return std::nullopt;
}

void DrawPipeline::addWrites(const std::vector<Range>& writes) {
    for (const auto& [begin, limit] : writes) {
        const auto held = std::find_if(queuedWrites.begin(), queuedWrites.end(), [&](const QueuedWrite& write) { return write.begin == begin && write.limit == limit; });
        if (held != queuedWrites.end()) ++held->count;
        else queuedWrites.push_back({begin, limit, 1});
    }
}

void DrawPipeline::removeWrites(const std::vector<Range>& writes) {
    for (const auto& [begin, limit] : writes) {
        const auto held = std::find_if(queuedWrites.begin(), queuedWrites.end(), [&](const QueuedWrite& write) { return write.begin == begin && write.limit == limit; });
        if (held == queuedWrites.end()) throw std::logic_error("draw pipeline write summary lost a queued write");
        if (--held->count != 0) continue;
        *held = queuedWrites.back();
        queuedWrites.pop_back();
    }
}

bool DrawPipeline::scanOverlaps(std::span<const Range> ranges) const {
    for (const auto& item : items) {
        for (const auto& [begin, limit] : item.writes) {
            for (const auto& [address, end] : ranges) {
                if (address < limit && begin < end) return true;
            }
        }
    }
    return false;
}

bool DrawPipeline::OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    static const bool verify = std::getenv("APS5_VERIFY_PIPELINE_WRITES") != nullptr;
    auto lock = lockQueue();
    bool overlaps = false;
    for (const auto& write : queuedWrites) {
        for (const auto& [address, end] : ranges) {
            if (address < write.limit && write.begin < end) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) break;
    }
    if (verify && overlaps != scanOverlaps(ranges)) throw std::logic_error("APS5_VERIFY_PIPELINE_WRITES: the queued write summary disagrees with the queued draws");
    return overlaps;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const std::array<Range, 1> range{Range{address, address + bytes}};
    return OverlapsAny(range);
}

}
