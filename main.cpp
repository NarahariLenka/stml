/**
 * @file main.cpp
 * @brief Scalable Thread Management Library (STML)
 * 
 * A single-file implementation of a C++ Thread Management Library.
 * Includes:
 * - Synchronization Primitives (Mutex, SpinLock, Semaphore, ConditionVariable)
 * - Thread Safe Task Queue
 * - Thread Pool
 * - Examples and Tests
 * 
 * Compilation:
 *   g++ -o stml main.cpp -std=c++17 -pthread
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <functional>
#include <atomic>
#include <future>
#include <chrono>
#include <sstream>
#include <cassert>

// ===========================================
// NAMESPACE: stml (Scalable Thread Management Library)
// ===========================================
namespace stml {

    // ===========================================
    // SYNCHRONIZATION MODULE
    // ===========================================

    /**
     * @brief Wrapper around std::mutex for standard locking.
     */
    class Mutex {
    public:
        void lock() { m_mutex.lock(); }
        void unlock() { m_mutex.unlock(); }
        bool try_lock() { return m_mutex.try_lock(); }
        
        // Expose underlying mutex for std::lock_guard compatibility if needed,
        // specifically for ConditionVariable.
        std::mutex& native_handle() { return m_mutex; }

    private:
        std::mutex m_mutex;
    };

    /**
     * @brief SpinLock using std::atomic_flag. 
     * Useful for short critical sections where putting thread to sleep is expensive.
     */
    class SpinLock {
    public:
        void lock() {
            while (m_flag.test_and_set(std::memory_order_acquire)) {
                // Busy-wait. In a real scenario, could use _mm_pause() or std::this_thread::yield()
                // to be friendlier to the CPU.
                std::this_thread::yield(); 
            }
        }

        void unlock() {
            m_flag.clear(std::memory_order_release);
        }

    private:
        std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
    };

    /**
     * @brief Semaphore implementation using C++17 standard primitives.
     * Allows controlling access to a pool of resources.
     */
    class Semaphore {
    public:
        explicit Semaphore(int count = 0) : m_count(count) {}

        void acquire() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_count > 0; });
            --m_count;
        }

        void release() {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_count;
            m_cv.notify_one();
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        int m_count;
    };

    /**
     * @brief Wrapper for std::condition_variable.
     */
    class ConditionVariable {
    public:
        void wait(std::unique_lock<std::mutex>& lock) {
            m_cv.wait(lock);
        }
        
        template <typename Predicate>
        void wait(std::unique_lock<std::mutex>& lock, Predicate pred) {
            m_cv.wait(lock, pred);
        }

        void notify_one() {
            m_cv.notify_one();
        }

        void notify_all() {
            m_cv.notify_all();
        }

    private:
        std::condition_variable m_cv;
    };

    // ===========================================
    // TASK QUEUE MODULE
    // ===========================================

    /**
     * @brief Thread-safe queue for holding tasks.
     */
    template <typename T>
    class TaskQueue {
    public:
        void push(T value) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push(std::move(value));
            }
            m_cv.notify_one();
        }

        bool try_pop(T& out_value) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty()) {
                return false;
            }
            out_value = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }

        void wait_and_pop(T& out_value) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return !m_queue.empty(); });
            out_value = std::move(m_queue.front());
            m_queue.pop();
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.empty();
        }

        size_t size() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::queue<T> m_queue;
    };

    // ===========================================
    // THREAD POOL MODULE
    // ===========================================

    /**
     * @brief Scalable Thread Pool.
     * Manages a set of worker threads to execute tasks concurrently.
     */
    class ThreadPool {
    public:
        explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency()) 
            : m_stop(false) 
        {
            if (num_threads == 0) num_threads = 2; // Fallback
            for (size_t i = 0; i < num_threads; ++i) {
                m_workers.emplace_back([this] {
                    this->worker_loop();
                });
            }
        }

        ~ThreadPool() {
            shutdown();
        }

        /**
         * @brief Submit a void function task to the pool.
         */
        void submit(std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                m_tasks.push(std::move(task));
            }
            m_cv.notify_one();
        }

        /**
         * @brief Gracefully shuts down the pool.
         * Waits for all threads to finish their current tasks (and potentially drain queue depending on implementation).
         * Here, we break the loop, so pending tasks in queue might not run if we don't drain. 
         * For robustness, we'll allow workers to drain the queue before exiting if desired,
         * but standard shutdown usually implies "stop accepting, stop working".
         * logic: set m_stop = true, notify all. Workers check m_stop and empty queue.
         */
        void shutdown() {
            {
                std::unique_lock<std::mutex> lock(m_queue_mutex);
                if (m_stop) return; // Already stopped
                m_stop = true;
            }
            m_cv.notify_all();
            for (std::thread& worker : m_workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            m_workers.clear();
        }

    private:
        void worker_loop() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_queue_mutex);
                    m_cv.wait(lock, [this] { 
                        return m_stop || !m_tasks.empty(); 
                    });

                    if (m_stop && m_tasks.empty()) {
                        return; // Exit thread
                    }

                    if (!m_tasks.empty()) {
                        task = std::move(m_tasks.front());
                        m_tasks.pop();
                    }
                }

                if (task) {
                    task();
                }
            }
        }

        std::vector<std::thread> m_workers;
        std::queue<std::function<void()>> m_tasks; // Using std::queue protected by mutex for simplicity in Pool
        std::mutex m_queue_mutex;
        std::condition_variable m_cv;
        bool m_stop;
    };

} // namespace stml

// ===========================================
// EXAMPLES & TESTS
// ===========================================

// ===========================================
// TEST REPORTING HELPER
// ===========================================

std::mutex g_print_mutex;
void safe_print(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << msg << std::endl;
}

struct TestStats {
    std::atomic<int> passed{0};
    std::atomic<int> failed{0};

    void record_pass(const std::string& name) {
        passed++;
        safe_print("[PASS] " + name);
    }

    void record_fail(const std::string& name, const std::string& reason) {
        failed++;
        safe_print("[FAIL] " + name + ": " + reason);
    }

    void print_summary() {
        safe_print("\n==============================");
        safe_print("         TEST SUMMARY         ");
        safe_print("==============================");
        safe_print("Total Tests: " + std::to_string(passed + failed));
        safe_print("Passed:      " + std::to_string(passed));
        safe_print("Failed:      " + std::to_string(failed));
        safe_print("==============================");
    }
};

void test_synchronization(TestStats& stats) {
    safe_print("\n[TEST] Running Synchronization Test (Semaphore)...");
    stml::Semaphore sem(2); // Allow 2 threads at a time
    std::vector<std::thread> threads;
    std::atomic<int> active_threads{0};
    std::atomic<bool> failure{false};

    auto job = [&](int id) {
        sem.acquire();
        active_threads++;
        // Verify we don't exceed 2
        int current = active_threads.load();
        if (current > 2) {
             safe_print("ERROR: More than 2 threads acquired semaphore!");
             failure = true;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        active_threads--;
        sem.release();
    };

    for (int i = 0; i < 6; ++i) {
        threads.emplace_back(job, i);
    }
    for (auto& t : threads) t.join();

    if (!failure) {
        stats.record_pass("Synchronization (Semaphore)");
    } else {
        stats.record_fail("Synchronization (Semaphore)", "Semaphore allowed more than 2 threads.");
    }
}

void test_thread_pool_basic(TestStats& stats, size_t num_threads) {
    safe_print("\n[TEST] Running Basic Thread Pool Test with " + std::to_string(num_threads) + " threads...");
    stml::ThreadPool pool(num_threads);
    
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&counter]() {
            counter++;
        });
    }

    // Give time to finish
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    if (counter == 100) {
        stats.record_pass("Thread Pool Basic (100 tasks) with " + std::to_string(num_threads) + " threads");
    } else {
        std::stringstream ss;
        ss << "Processed " << counter << " tasks, expected 100.";
        stats.record_fail("Thread Pool Basic", ss.str());
    }
}

void test_stress_scaling(TestStats& stats, size_t num_threads) {
    safe_print("\n[TEST] Running Stress Test (10,000 Tasks) with " + std::to_string(num_threads) + " threads...");
    stml::ThreadPool pool(num_threads);
    std::atomic<int> counter{0};
    const int task_count = 10000;

    for (int i = 0; i < task_count; ++i) {
        pool.submit([&counter]() {
            // Do some "work"
            int volatile val = 0; 
            for(int j=0; j<100; ++j) val += j;
            counter++;
        });
    }

    // Wait until completion or timeout (simple spin wait for test)
    int retries = 0;
    while (counter < task_count && retries < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries++;
    }

    // Force wait a bit more if needed or just check
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int final_count = counter.load();
    std::stringstream ss;
    ss << "Tasks Completed: " << final_count << "/" << task_count;
    safe_print(ss.str());

    if (final_count == task_count) {
        stats.record_pass("Stress Test (10k tasks) with " + std::to_string(num_threads) + " threads");
    } else {
        stats.record_fail("Stress Test", "Count did not reach target, possible timeout or drop.");
    }
}

int main() {
    safe_print("==========================================");
    safe_print(" Scalable Thread Management Library (STML)");
    safe_print("==========================================");

    size_t num_threads;
    std::cout << "Enter number of threads for thread pool: ";
    if (!(std::cin >> num_threads) || num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 2;
        std::cout << "Invalid or zero input. Using default: " << num_threads << std::endl;
        // clear error state
        std::cin.clear();
        // Ignore implies we include limits but for simplicity just clearing flag often enough if next simple read, 
        // but better correct:
        // std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        // Since we don't read anything else, we can skip complex ignore logic or just do a simple char eat loop if needed, 
        // but simple fallback is fine here.
    }

    TestStats stats;

    test_synchronization(stats);
    test_thread_pool_basic(stats, num_threads);
    test_stress_scaling(stats, num_threads);

    stats.print_summary();
    safe_print("\nAll demos finished. Exiting...");
    return 0;
}
