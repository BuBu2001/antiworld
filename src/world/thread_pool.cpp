#include "world/thread_pool.h"

#include <algorithm>

namespace world {

ThreadPool::ThreadPool(unsigned threadCount) {
    unsigned desired = threadCount;
    if (desired == 0) {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        // Оставляем главному потоку (рендер/физика) его ядро: генерация не
        // должна выжирать все логические процессоры и сама провоцировать
        // фризы планировщиком.
        desired = hw > 2 ? hw - 1 : hw;
    }

    workers_.reserve(desired);
    try {
        for (unsigned i = 0; i < desired; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    } catch (const std::exception&) {
        // Если удалось поднять не все потоки — корректно гасим поднятые
        // (деструктор сам себя вызовет только для полностью сконструированного
        // объекта, поэтому здесь делаем эквивалентную остановку вручную).
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    // notify_all, а не notify_one: все воркеры должны проснуться, чтобы
    // каждый из них смог проверить очередь и выйти.
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

std::size_t ThreadPool::pendingTasks() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Предикатная версия wait(): устойчива к ложным пробуждениям.
            // При stop_ не выходим немедленно, пока очередь не пуста — так
            // деструктор гарантированно дожидается всех поставленных задач.
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return;  // stop_ && очередь пуста
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        // Выполняем БЕЗ замка: один воркер на задаче, остальные продолжают
        // брать новые. Исключения из задачи уходит в promise (packaged_task),
        // до потока они не доходят.
        task();
    }
}

}  // namespace world
