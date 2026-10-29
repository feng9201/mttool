#ifndef MTTOOL_THREAD_POOL_H
#define MTTOOL_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mttool {

class ThreadPool {
public:
    static constexpr std::size_t kFallbackThreadCount = 4;

    // 线程数不再受硬上限约束；默认取硬件并发度，取不到时用 4。
    static std::size_t DefaultThreadCount() noexcept {
        const unsigned hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<std::size_t>(hw) : kFallbackThreadCount;
    }

    explicit ThreadPool(std::size_t threadCount = DefaultThreadCount()) {
        if (threadCount == 0) threadCount = 1;
        m_threads.reserve(threadCount);
        try {
            for (std::size_t i = 0; i < threadCount; ++i) {
                m_threads.emplace_back(&ThreadPool::WorkerLoop, this);
            }
        } catch (...) {
            Shutdown(false);
            throw;
        }
    }

    // 默认优雅退出：不再接收新任务，把手上的任务跑完再回收线程。
    ~ThreadPool() { Shutdown(true); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // 直接支持成员函数、函数对象、lambda；参数支持 move-only 类型。
    template <class F, class... Args>
    auto Submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [fn = std::forward<F>(f), bound = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
                return std::apply(
                    [&fn](auto&&... a) -> Result {
                        return std::invoke(std::move(fn), std::move(a)...);
                    },
                    std::move(bound));
            });

        std::future<Result> future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                throw std::runtime_error("mttool::ThreadPool: Submit on a closed pool");
            }
            m_tasks.emplace_back([task] { (*task)(); });
        }
        m_cv.notify_one();
        return future;
    }

    // 停止接收新任务；已入队的任务继续执行，线程不退出，因此可以 ReOpen。
    void Close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }

    // 只对 Close() 有效；Shutdown 之后线程已回收，不能再打开。
    bool ReOpen() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return false;
        m_closed = false;
        return true;
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    std::size_t GetTaskSize() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }

    std::size_t ThreadCount() const noexcept { return m_threads.size(); }

    // 当前没有在跑任务的线程数；排队中的任务不算忙。
    std::size_t IdleCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_threads.size() - m_busy;
    }

    // 等到队列空且没有任务在执行。不要在任务内部调用，会自等。
    void Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idleCv.wait(lock, [this] { return m_tasks.empty() && m_busy == 0; });
    }

    // drain = true 跑完剩余任务，false 直接丢弃。返回 false 表示已经停止过。
    // 不要在任务内部调用，会 join 自己。
    bool Shutdown(bool drain = true) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return false;
            if (!drain) m_tasks.clear();
            m_shutdown = true;
            m_closed = true;
        }
        m_cv.notify_all();
        for (std::thread& thread : m_threads) {
            if (thread.joinable()) thread.join();
        }
        return true;
    }

private:
    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_shutdown || !m_tasks.empty(); });
                if (m_shutdown && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
                ++m_busy;
            }

            task();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_busy;
                if (m_tasks.empty() && m_busy == 0) m_idleCv.notify_all();
            }
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_threads;
    std::size_t m_busy = 0;
    bool m_closed = false;
    bool m_shutdown = false;
};

inline ThreadPool& PoolInstance(std::size_t threadCount = ThreadPool::DefaultThreadCount()) {
    static ThreadPool pool(threadCount);
    return pool;
}

}  // namespace mttool

#endif  // MTTOOL_THREAD_POOL_H
