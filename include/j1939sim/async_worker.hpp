#ifndef ASYNC_WORKER_HPP
#define ASYNC_WORKER_HPP

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <atomic>

namespace j1939sim
{

    class AsyncWorker
    {
    public:
        using TaskId = uint64_t;
        using Clock = std::chrono::steady_clock;
        using Task = std::function<void()>;

        AsyncWorker(size_t thread_count = 1) : running_(true)
        {
            for (size_t i = 0; i < thread_count; ++i)
            {
                workers_.emplace_back([this]
                                      { workerLoop(); });
            }
        }

        ~AsyncWorker()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
            }
            condition_.notify_all();
            for (auto &thread : workers_)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
        }

        // 提交任务，可以指定延迟执行时间
        TaskId submit(Task task, std::chrono::milliseconds delay = std::chrono::milliseconds(0))
        {
            TaskId id = next_task_id_++;
            TaskItem item{id, std::move(task), Clock::now() + delay};

            {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks_.push(std::move(item));
            }
            condition_.notify_one();
            return id;
        }

        // 取消任务
        bool cancel(TaskId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 在任务队列中查找并移除任务
            std::queue<TaskItem> temp;
            bool found = false;
            while (!tasks_.empty())
            {
                auto &item = tasks_.front();
                if (item.id != id)
                {
                    temp.push(std::move(item));
                }
                else
                {
                    found = true;
                }
                tasks_.pop();
            }
            tasks_ = std::move(temp);
            return found;
        }

    private:
        struct TaskItem
        {
            TaskId id;
            Task task;
            Clock::time_point execute_time;

            bool operator>(const TaskItem &other) const
            {
                return execute_time > other.execute_time;
            }
        };

        void workerLoop()
        {
            while (true)
            {
                TaskItem task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this]
                                    { return !running_ || !tasks_.empty(); });

                    if (!running_ && tasks_.empty())
                    {
                        return;
                    }

                    if (tasks_.empty())
                    {
                        continue;
                    }

                    auto now = Clock::now();
                    if (tasks_.front().execute_time > now)
                    {
                        condition_.wait_until(lock, tasks_.front().execute_time);
                        continue;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                // 执行任务
                try
                {
                    task.task();
                }
                catch (...)
                {
                    // 记录错误但继续运行
                }
            }
        }

        std::vector<std::thread> workers_;
        std::queue<TaskItem> tasks_;
        std::mutex mutex_;
        std::condition_variable condition_;
        std::atomic<bool> running_;
        std::atomic<TaskId> next_task_id_{0};
    };

} // namespace j1939sim

#endif // ASYNC_WORKER_HPP
