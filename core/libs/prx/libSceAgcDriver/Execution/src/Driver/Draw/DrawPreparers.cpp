#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPreparers.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace AgcDriver::DriverDetail {

namespace {

thread_local bool preparerThread = false;
thread_local bool holdsTurn = false;
thread_local std::uint64_t currentTicket = 0;
thread_local std::uint64_t turnWaitNs = 0;
thread_local std::uint64_t preparedFrom = 0;
thread_local std::unique_ptr<DrawPreparers::Result> finished;

std::chrono::nanoseconds waitSpin() {
    static const std::chrono::nanoseconds window = [] {
        const char* text = std::getenv("APS5_DRAW_PREPARE_SPIN_US");
        if (text != nullptr) return std::chrono::nanoseconds(std::chrono::microseconds(std::clamp<long long>(std::strtoll(text, nullptr, 10), 0, 10000)));
        return DrawPreparers::Threads() > 1 ? std::chrono::nanoseconds::zero() : HandoffSpinWindow();
    }();
    return window;
}

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}

DrawPreparers& DrawPreparers::Queue0() {
    static DrawPreparers* preparers = new DrawPreparers();
    return *preparers;
}

unsigned DrawPreparers::Threads() {
    static const unsigned threads = [] {
        const char* text = std::getenv("APS5_DRAW_PREPARE_THREADS");
        if (text == nullptr || DrawPipeline::Depth() == 0) return 0u;
        const auto value = std::strtoul(text, nullptr, 10);
        return static_cast<unsigned>(std::min<unsigned long>(value, std::max(1u, UsableProcessors())));
    }();
    return threads;
}

std::size_t DrawPreparers::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_DRAW_PREPARE_DEPTH");
        if (text == nullptr) return std::size_t{64};
        return static_cast<std::size_t>(std::clamp<unsigned long long>(std::strtoull(text, nullptr, 10), 1, 1024));
    }();
    return depth;
}

bool DrawPreparers::OnPreparer() {
    return preparerThread;
}

void DrawPreparers::OnIdle(std::function<void()> release) {
    idle = std::move(release);
}

void DrawPreparers::start() {
    std::fprintf(stderr, "[draw] draws are prepared on %u threads (APS5_DRAW_PREPARE_THREADS), %zu handed over at most\n", Threads(), Depth());
    for (unsigned i = 0; i < Threads(); ++i) threads.emplace_back([this] { run(); });
}

void DrawPreparers::Submit(const QueueState& queue, Job job) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (threads.empty()) start();
    if (failed.load(std::memory_order_acquire)) rethrowFailure();
    std::unique_ptr<QueueState> state;
    {
        LockSpinning(mutex, HandoffSpinWindow());
        std::unique_lock lock(mutex, std::adopt_lock);
        const auto full = [&] { return submitted.load(std::memory_order_relaxed) - turn.load(std::memory_order_acquire) >= Depth(); };
        if (full()) {
            const auto start = std::chrono::steady_clock::now();
            roomWaiting = true;
            room.wait(lock, [&] { return !full(); });
            roomWaiting = false;
            fullNs += elapsedNs(start);
        }
        if (!spare.empty()) {
            state = std::move(spare.back());
            spare.pop_back();
        }
    }
    const auto copyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (state == nullptr) state = std::make_unique<QueueState>(queue);
    else *state = queue;
    const auto copied = profile ? elapsedNs(copyStart) : 0;
    LockSpinning(mutex, HandoffSpinWindow());
    std::unique_lock lock(mutex, std::adopt_lock);
    const auto ticket = submitted.load(std::memory_order_relaxed);
    items.push_back({ticket, std::move(state), std::move(job)});
    slots.emplace_back();
    submitted.store(ticket + 1, std::memory_order_release);
    copyNs += copied;
    depthSum += items.size();
    if (sleepers != 0) wake.notify_one();
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPreparers::Wait(std::uint32_t opcode) {
    if (!Busy()) {
        if (failed.load(std::memory_order_acquire)) rethrowFailure();
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto target = submitted.load(std::memory_order_acquire);
    const auto done = [&] { return turn.load(std::memory_order_acquire) >= target; };
    std::unique_lock lock(mutex, std::defer_lock);
    if (!SpinUntil(waitSpin() * 2, done)) {
        lock.lock();
        ++turnWaiters;
        turned.wait(lock, done);
        --turnWaiters;
    } else {
        lock.lock();
    }
    ++waits;
    waitNs += elapsedNs(start);
    ++waitOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    lock.unlock();
    if (failed.load(std::memory_order_acquire)) rethrowFailure();
}

void DrawPreparers::rethrowFailure() {
    std::unique_lock lock(mutex);
    const auto target = submitted.load(std::memory_order_acquire);
    ++turnWaiters;
    turned.wait(lock, [&] { return turn.load(std::memory_order_acquire) >= target; });
    --turnWaiters;
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    skipping = false;
    failed.store(false, std::memory_order_release);
    lock.unlock();
    std::rethrow_exception(error);
}

void DrawPreparers::AwaitTurn() {
    if (!preparerThread || holdsTurn) return;
    const auto ticket = currentTicket;
    const auto mine = [&] { return turn.load(std::memory_order_acquire) == ticket; };
    if (!mine()) {
        const auto start = std::chrono::steady_clock::now();
        if (!SpinUntil(waitSpin(), mine)) {
            std::unique_lock lock(mutex);
            ++turnWaiters;
            turned.wait(lock, mine);
            --turnWaiters;
        }
        turnWaitNs += elapsedNs(start);
    }
    holdsTurn = true;
}

void DrawPreparers::NoteWrites(const std::vector<Range>& writes) {
    if (!preparerThread || Threads() < 2) return;
    LockSpinning(mutex, HandoffSpinWindow());
    std::lock_guard lock(mutex, std::adopt_lock);
    noteWrites(currentTicket, writes);
}

void DrawPreparers::noteWrites(std::uint64_t ticket, const std::vector<Range>& writes) {
    if (written.empty()) writtenFrom = ticket;
    while (writtenFrom + written.size() < ticket) written.emplace_back(writtenFrom + written.size(), std::vector<Range>{});
    if (writtenFrom + written.size() == ticket) written.emplace_back(ticket, writes);
    else {
        auto& slot = written[ticket - writtenFrom].second;
        slot.insert(slot.end(), writes.begin(), writes.end());
    }
    const auto keep = 4 * Depth() + Threads();
    while (written.size() > keep) {
        written.pop_front();
        ++writtenFrom;
    }
}

bool DrawPreparers::writtenSince(std::span<const Range> reads, std::uint64_t from, std::uint64_t ticket) const {
    if (from >= ticket) return false;
    if (from < writtenFrom && !written.empty()) return true;
    for (const auto& [logged, writes] : written) {
        if (logged < from || logged >= ticket) continue;
        for (const auto& [begin, limit] : writes) {
            for (const auto& [address, end] : reads) {
                if (address < limit && begin < end) return true;
            }
        }
    }
    return false;
}

bool DrawPreparers::Finish(Commit commit, std::vector<Range> writes, std::vector<Range> reads, std::vector<Range> preparedReads) {
    if (holdsTurn) {
        bool stale = false;
        {
            LockSpinning(mutex, HandoffSpinWindow());
            std::lock_guard lock(mutex, std::adopt_lock);
            stale = writtenSince(preparedReads, preparedFrom, currentTicket);
            if (stale) ++redone;
        }
        preparedFrom = currentTicket;
        if (stale) return true;
        DrawPipeline::Queue0().Enqueue(std::move(commit), std::move(writes), 0, {}, std::move(reads));
        return false;
    }
    finished = std::make_unique<Result>(Result{std::move(commit), std::move(writes), std::move(reads), std::move(preparedReads), preparedFrom});
    return false;
}

void DrawPreparers::passTurn(std::uint64_t ticket) {
    turn.store(ticket + 1, std::memory_order_release);
    if (turnWaiters != 0) turned.notify_all();
    if (roomWaiting) room.notify_one();
}

void DrawPreparers::run() {
    preparerThread = true;
    DrawPipeline::Active() = true;
    PerformanceTrace::BindThread("draw preparer");
    try {
        RaiseWorkerThreadPriority("draw preparer");
        PinDrawThread("draw preparer");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[draw] preparer priority not raised: %s\n", error.what());
    }
    for (;;) {
        Item item;
        bool skip = false;
        {
            LockSpinning(mutex, HandoffSpinWindow());
            std::unique_lock lock(mutex, std::adopt_lock);
            if (items.empty() && !closing) {
                const auto seen = submitted.load(std::memory_order_acquire);
                lock.unlock();
                if (!SpinUntil(waitSpin(), [&] { return submitted.load(std::memory_order_acquire) != seen; }) && idle) idle();
                lock.lock();
            }
            ++sleepers;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            --sleepers;
            if (items.empty()) {
                lock.unlock();
                if (idle) idle();
                break;
            }
            item = std::move(items.front());
            items.pop_front();
            skip = skipping;
        }
        currentTicket = item.ticket;
        holdsTurn = false;
        preparedFrom = turn.load(std::memory_order_acquire);
        turnWaitNs = 0;
        const auto start = std::chrono::steady_clock::now();
        std::exception_ptr error;
        if (!skip) {
            try {
                item.job(*item.state);
            } catch (...) {
                error = std::current_exception();
            }
        }
        const auto spent = elapsedNs(start);
        LockSpinning(mutex, HandoffSpinWindow());
        std::unique_lock lock(mutex, std::adopt_lock);
        if (error != nullptr) {
            if (failure == nullptr) failure = error;
            skipping = true;
            failed.store(true, std::memory_order_release);
        }
        auto& slot = slots[item.ticket - turn.load(std::memory_order_relaxed)];
        slot.done = true;
        if (finished != nullptr) {
            slot.result = std::move(finished);
            slot.item = std::move(item);
            ++parked;
        } else {
            spare.push_back(std::move(item.state));
        }
        ++jobs;
        jobNs += spent;
        turnNs += turnWaitNs;
        if (!flushing && !slots.empty() && slots.front().done) flush(lock);
    }
}

bool DrawPreparers::flushBatch(std::unique_lock<std::mutex>& lock) {
    const auto first = turn.load(std::memory_order_relaxed);
    std::vector<DrawPipeline::Handed> batch;
    std::size_t count = 0;
    for (; count < slots.size() && slots[count].done; ++count) {
        auto& result = slots[count].result;
        if (result == nullptr) continue;
        if (writtenSince(result->preparedReads, result->preparedFrom, first + count)) break;
        noteWrites(first + count, result->writes);
        batch.push_back({std::move(result->commit), std::move(result->writes), std::move(result->reads)});
    }
    if (batch.empty()) return false;
    lock.unlock();
    std::exception_ptr error;
    const auto savedTicket = currentTicket;
    const auto savedHolds = holdsTurn;
    currentTicket = first;
    holdsTurn = true;
    try {
        DrawPipeline::Queue0().EnqueueBatch(batch);
    } catch (...) {
        error = std::current_exception();
    }
    currentTicket = savedTicket;
    holdsTurn = savedHolds;
    batch.clear();
    lock.lock();
    if (error != nullptr) {
        if (failure == nullptr) failure = error;
        skipping = true;
        failed.store(true, std::memory_order_release);
    }
    ++batches;
    for (std::size_t i = 0; i < count; ++i) {
        auto& slot = slots.front();
        if (slot.result != nullptr) ++batched;
        slot.result.reset();
        slot.item.job = nullptr;
        if (slot.item.state != nullptr) spare.push_back(std::move(slot.item.state));
        const auto ticket = turn.load(std::memory_order_relaxed);
        slots.pop_front();
        passTurn(ticket);
    }
    return true;
}

void DrawPreparers::flush(std::unique_lock<std::mutex>& lock) {
    static const bool batching = [] {
        const char* value = std::getenv("APS5_DRAW_PREPARE_BATCH");
        return value != nullptr && std::string(value) == "1";
    }();
    flushing = true;
    while (!slots.empty() && slots.front().done) {
        if (batching && flushBatch(lock)) continue;
        const auto ticket = turn.load(std::memory_order_relaxed);
        auto result = std::move(slots.front().result);
        auto item = std::move(slots.front().item);
        if (result != nullptr) {
            const bool stale = writtenSince(result->preparedReads, result->preparedFrom, ticket);
            if (stale) ++redone;
            lock.unlock();
            std::exception_ptr error;
            const auto savedTicket = currentTicket;
            const auto savedHolds = holdsTurn;
            currentTicket = ticket;
            holdsTurn = true;
            preparedFrom = ticket;
            try {
                if (stale) item.job(*item.state);
                else DrawPipeline::Queue0().Enqueue(std::move(result->commit), std::move(result->writes), 0, {}, std::move(result->reads));
            } catch (...) {
                error = std::current_exception();
            }
            finished.reset();
            currentTicket = savedTicket;
            holdsTurn = savedHolds;
            result.reset();
            item.job = nullptr;
            lock.lock();
            if (error != nullptr) {
                if (failure == nullptr) failure = error;
                skipping = true;
                failed.store(true, std::memory_order_release);
            }
            if (item.state != nullptr) spare.push_back(std::move(item.state));
        }
        slots.pop_front();
        passTurn(ticket);
    }
    flushing = false;
}

void DrawPreparers::Stop() {
    std::vector<std::thread> stopping;
    {
        std::lock_guard lock(mutex);
        closing = true;
        stopping = std::move(threads);
    }
    wake.notify_all();
    for (auto& thread : stopping) {
        if (thread.get_id() == std::this_thread::get_id()) thread.detach();
        else if (thread.joinable()) thread.join();
    }
}

void DrawPreparers::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string byOpcode;
    for (int shown = 0; shown < 6; ++shown) {
        const auto top = std::max_element(waitOpcodes.begin(), waitOpcodes.end());
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - waitOpcodes.begin()), static_cast<unsigned long long>(*top));
        byOpcode += item;
        *top = 0;
    }
    waitOpcodes = {};
    const auto per = [](std::uint64_t ns, std::uint64_t count) { return count == 0 ? 0.0 : static_cast<double>(ns) / 1e3 / static_cast<double>(count); };
    const auto submittedNow = submitted.load(std::memory_order_relaxed);
    static std::uint64_t submittedBefore = 0;
    const auto handed = submittedNow - submittedBefore;
    submittedBefore = submittedNow;
    AgcDriver::ProfilePrint_nid_no_patch("[draw] preparers (10 s, %u threads, depth %zu, spin %lld us): %llu draws handed over, %llu prepared at %.1f us each, waiting %.2f us each for their turn, %llu parked for their turn, %llu prepared again in order (an earlier draw wrote what they read), %llu handed over in %llu batches; handing over: state copy %.2f us, waiting %.2f us for room, behind %.1f on average; %llu waits for the preparers (%.1f ms), by opcode:%s\n", Threads(), Depth(), static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(waitSpin()).count()), static_cast<unsigned long long>(handed), static_cast<unsigned long long>(jobs), per(jobNs, jobs), per(turnNs, jobs), static_cast<unsigned long long>(parked), static_cast<unsigned long long>(redone), static_cast<unsigned long long>(batched), static_cast<unsigned long long>(batches), per(copyNs, handed), per(fullNs, handed), handed == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(handed), static_cast<unsigned long long>(waits), static_cast<double>(waitNs) / 1e6, byOpcode.c_str());
    jobs = jobNs = turnNs = copyNs = fullNs = waits = waitNs = depthSum = redone = parked = batched = batches = 0;
}

}
