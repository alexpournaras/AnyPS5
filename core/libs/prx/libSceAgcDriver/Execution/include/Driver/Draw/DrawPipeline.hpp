#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWPIPELINE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace AgcDriver::DriverDetail {

// APS5_DRAW_PIPELINE=1: queue 0's worker prepares a draw (decode, cache lookup, shader capture and
// specialization) and hands its recording (GpuMutex, VulkanDevice::Draw) to one recording thread,
// so the preparation of a draw overlaps the recording of the ones before it. Tasks run in the order
// they were handed over, and the worker drains them before it takes GpuMutex itself
// (GuestMemory::SetGpuLockHook): labels, submits, dispatches and flips reach the device in the
// order they would without the pipeline.
class DrawPipeline {
public:
    static bool Enabled();
    // Makes `pipeline` (null: none) the one the GpuMutex lock hook drains.
    static void Activate(DrawPipeline* pipeline);
    // Whether the calling thread handed over tasks that have not finished (false on any other
    // thread, the recording thread included).
    bool PendingForThisThread() const;
    // Runs `task` on the recording thread after the tasks handed over before it; blocks while
    // Capacity tasks are waiting. One thread hands tasks over.
    void Enqueue(std::function<void()> task);
    // Waits until every task handed over has finished.
    void Drain();
    // Ends the recording thread; tasks not yet started are dropped.
    void Stop();
    ~DrawPipeline();

private:
    static constexpr std::size_t Capacity = 256;
    void run(std::stop_token token);

    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<std::function<void()>> tasks;
    std::atomic<std::uint64_t> outstanding{0};
    std::atomic<std::thread::id> producer{};
    std::jthread thread;
};

}

#endif
