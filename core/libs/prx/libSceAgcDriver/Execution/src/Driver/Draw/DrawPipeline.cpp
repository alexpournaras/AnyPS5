#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace AgcDriver::DriverDetail {

bool DrawPipeline::Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_DRAW_PIPELINE");
        return text != nullptr && text[0] != '\0' && text[0] != '0';
    }();
    return enabled;
}

namespace {
std::atomic<DrawPipeline*> active{nullptr};

void drainBeforeGpuLock() {
    auto* pipeline = active.load(std::memory_order_acquire);
    if (pipeline != nullptr && pipeline->PendingForThisThread()) pipeline->Drain();
}
}

void DrawPipeline::Activate(DrawPipeline* pipeline) {
    static const bool installed = (GuestMemory::SetGpuLockHook(&drainBeforeGpuLock), true);
    (void)installed;
    active.store(pipeline, std::memory_order_release);
}

bool DrawPipeline::PendingForThisThread() const {
    return outstanding.load(std::memory_order_acquire) != 0 && producer.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

void DrawPipeline::Enqueue(std::function<void()> task) {
    std::unique_lock lock(mutex);
    if (!thread.joinable()) {
        producer.store(std::this_thread::get_id(), std::memory_order_relaxed);
        std::fprintf(stderr, "[draw-pipeline] draws are recorded on their own thread (APS5_DRAW_PIPELINE)\n");
        thread = std::jthread([this](std::stop_token token) { run(token); });
    }
    changed.wait(lock, [&] { return tasks.size() < Capacity; });
    tasks.push_back(std::move(task));
    outstanding.fetch_add(1, std::memory_order_acq_rel);
    lock.unlock();
    changed.notify_all();
}

void DrawPipeline::Drain() {
    if (outstanding.load(std::memory_order_acquire) == 0) return;
    std::unique_lock lock(mutex);
    const auto drained = [&] { return outstanding.load(std::memory_order_acquire) == 0; };
    if (changed.wait_for(lock, std::chrono::seconds(5), drained)) return;
    std::fprintf(stderr, "[draw-pipeline] waited 5 s for %llu recorded draws to finish\n", static_cast<unsigned long long>(outstanding.load()));
    changed.wait(lock, drained);
}

void DrawPipeline::Stop() {
    if (!thread.joinable()) return;
    thread.request_stop();
    changed.notify_all();
    thread.join();
    std::lock_guard lock(mutex);
    outstanding.fetch_sub(tasks.size(), std::memory_order_acq_rel);
    tasks.clear();
    changed.notify_all();
}

DrawPipeline::~DrawPipeline() {
    Stop();
}

void DrawPipeline::run(std::stop_token token) {
    PinWorkerThread("draw recorder");
    // Charged to queue 0, whose draws it records.
    GuestMemory::TagGpuLockThread(0);
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex);
            if (!changed.wait(lock, token, [&] { return !tasks.empty(); })) return;
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        changed.notify_all();
        // A task reports its own failure (Driver::ReportFailure); one that escapes is a bug.
        try {
            task();
        } catch (...) {
            std::terminate();
        }
        task = nullptr;
        {
            std::lock_guard lock(mutex);
            outstanding.fetch_sub(1, std::memory_order_acq_rel);
        }
        changed.notify_all();
    }
}

}
