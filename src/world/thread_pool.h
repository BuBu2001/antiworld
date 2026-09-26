#pragma once

// Минимальный пул рабочих потоков для фоновой генерации чанков (CPU-side).
//
// Зачем он нужен: мир площадью ~510 млн км² стримится порциями, и генерация
// карты высот + mesh одного чанка — это миллионы арифметических операций.
// Делать это в главном потоке нельзя: будут фризы. Поэтому главный поток
// только кладёт задачи сюда и забирает готовые результаты; сами задачи
// выполняются воркерами.
//
// Механика:
//   * очередь задач под std::mutex, воркеры ждут на std::condition_variable;
//   * submit() принимает callable и возвращает std::future — так вызывающий
//     может не блокироваться, а лишь периодически проверять готовность;
//   * деструктор останавливает пул: ждёт завершения ВСЕХ взятых в работу
//     задач (включая поставленные в очередь, но ещё не начатые — они тоже
//     отрабатывают), затем присоединяет потоки. Это гарантирует, что к моменту
//     уничтожения пула ни один future не «висит» на незавершённой задаче.
//
// Потокобезопасность: submit() можно вызывать из любого потока (главного).
// Задачи обязаны быть исключения-безопасными или бросать только то, что
// устроено перенести через future (promise это делает автоматически).

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace world {

class ThreadPool {
public:
    // threadCount = 0 означает hardware_concurrency (с разумным минимумом 1).
    explicit ThreadPool(unsigned threadCount = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Кладёт задачу в очередь, возвращает future с её результатом.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using Result = std::invoke_result_t<F, Args...>;

        // packaged_task + shared_ptr: очередь должна хранить копируемый
        // std::function, а packaged_task перемещаемый, но не копируемый.
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<Result> future = task->get_future();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    unsigned threadCount() const noexcept {
        return static_cast<unsigned>(workers_.size());
    }
    // Число задач, которые ещё не начали выполняться.
    std::size_t pendingTasks() const;

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_{false};
};

}  // namespace world
