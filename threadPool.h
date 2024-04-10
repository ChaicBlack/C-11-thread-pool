#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
private:
  std::vector<std::thread> workers;
  std::queue<std::function<void()>> tasks;

  std::mutex queue_mutex;
  std::condition_variable condition;
  bool stop;
public:
  ThreadPool(size_t);
  template <class F, class... Args>
  auto enqueue(F &&f, Args &&...args)
      -> std::future<typename std::result_of<F(Args...)>::type>;
  ~ThreadPool();
};

inline ThreadPool::ThreadPool(size_t threads) : stop(false) {
  for (size_t i = 0; i < threads; ++i)
    workers.emplace_back([this] {
      // worker线程一直自循环尝试拿任务
      for (;;) {
        std::function<void()> task;

        {
          std::unique_lock<std::mutex> lock(this->queue_mutex);
          // 线程运行到这里会自动阻塞并释放锁，只有当enqueue或者
          // 析构函数被调用的时候，才能unblock并尝试获取锁执行下面的代码
          this->condition.wait(
              lock, [this] { return this->stop || !this->tasks.empty(); });
          // stop=1而且任务被拿完了以后，当前线程就结束了
          if (this->stop && this->tasks.empty())
            return;
          // 从任务列表里拿任务
          task = std::move(this->tasks.front());
          this->tasks.pop();
        }

        //执行任务
        task();
      }
    });
}

template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args)
    -> std::future<typename std::result_of<F(Args...)>::type> {
  // 让future那一行更加可读
  using return_type = typename std::result_of<F(Args...)>::type;

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock<std::mutex> lock(queue_mutex);

    // stop=true时不准再加任务了
    if (stop)
      throw std::runtime_error("enqueue on stopped ThreadPool");

    tasks.emplace([task]() { (*task)(); });
  }
  // 通知一个worker来领task
  condition.notify_one();
  return res;
}

// 析构函数需要join每个线程
inline ThreadPool::~ThreadPool() {
  {
    std::unique_lock<std::mutex> lock(queue_mutex);
    stop = true;
  }

  // 通知每个线程去领任务或者结束
  condition.notify_all();
  for (std::thread &worker : workers)
    worker.join();
}

#endif
