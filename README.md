# mttool

mttool 是一个 C++17 头文件库集合，包含以下组件：

## include/mttool — 自封装工具库

自封装的单头文件工具库，包含：

- **ThreadPool.h** — 线程池，用于高效管理任务队列和线程资源
- **ThreadTimer.h** — 线程定时器，支持定时任务调度
- **TimeTicker.h** — 时间计时类，用于高精度时间测量
- **onceToken.h** — 一次性执行令牌
- **BS_thread_pool.h** — 开源线程池(https://github.com/bshoshany/thread-pool)

## include/eventpp — 事件调度库

开源事件调度库 [eventpp](https://github.com/wqking/eventpp)，提供灵活的事件派发和回调管理机制。

- **CallbackList** — 回调列表
- **EventDispatcher** — 事件派发器
- **EventQueue** — 事件队列（支持异步事件处理）
- **HeterCallbackList / HeterEventDispatcher / HeterEventQueue** — 异构类型版本
- **Mixin 机制** — 过滤器等扩展支持

## include/spsc — 单生产者单消费者无锁队列

基于 [readerwriterqueue](https://github.com/cameron314/readerwriterqueue) 的单生产者单消费者无锁队列，提供高效的线程间数据传递。

- **ReaderWriterQueue** — 单生产者单消费者无锁队列
- **ReaderWriterCircularBuffer** — 循环缓冲区版本

## include/mpmc — 多生产者多消费者无锁队列

基于 [concurrentqueue](https://github.com/cameron314/concurrentqueue) 的多生产者多消费者无锁队列。

- **ConcurrentQueue** — 多生产者多消费者无锁队列
- **BlockingConcurrentQueue** — 阻塞版本，支持等待操作
- **LightweightSemaphore** — 轻量级信号量


## include/magic_enum — 枚举与字符串之间
基于 [magic_enum](https://github.com/Neargye/magic_enum) 开源框架解决枚举与字符串之间的转换。


## 构建

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## 依赖

- C++17 兼容编译器