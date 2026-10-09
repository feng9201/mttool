# observer_list_modern

对 Chromium 最新 `base/observer_list.h` 逻辑的 **C++17/C++20 双轨复刻**（header-only 库 + CMake 管理 + demo 自检）。
同一份源码：编译器支持 C++20 时走 concepts/ranges 路径，否则自动降级为 SFINAE 路径，语义完全一致。

## 计划背景

评估结论：最新逻辑约 90%+ 可用原生 C++17 实现，
仅 5 处需舍弃/降级（`raw_ptr`/MiraclePtr、`DUMP_WILL_BE_*` 灰度、SequenceChecker、concepts 报错质量、DCHECK 注册栈符号化）。

## 目录结构

```
observer_list_modern/
├── CMakeLists.txt                  # 独立工程，不挂入主工程根 CMakeLists
├── PLAN.md                         # 多源版增强计划
├── include/observer_list/
│   ├── port.h                      # C++20 探测宏 + CHECK/DCHECK 断言宏（可配置裁决）
│   ├── observer_list_types.h       # CheckedObserver（LifetimeToken 防悬垂）
│   ├── observer_list_internal.h    # WeakLinkNode 侵入式链表节点 + 存储适配器
│   ├── observer_list.h             # ObserverList 主体（Notify 双轨实现）
│   ├── scoped_observation.h        # ScopedObservation / ScopedMultiSourceObservation RAII
│   └── event_channel.h             # Channel<Msg> typed EventBus（替代 NotificationService）
└── demo/
    └── main.cpp                    # 使用 demo + 内置自检（CTest 注册，非 0 退出码即失败）
```

## 双轨机制

1. **编译期探测（port.h）**：兼容 MSVC 的 `_MSVC_LANG`，`OBS_CXX_LANG >= 202002L` 且未定义
   `OBSERVER_LIST_DISABLE_CPP20` 时启用 `OBS_HAS_CPP20`。
2. **CMake 探针**：`check_cxx_source_compiles` 用最小 `concept` 用例探测编译器真实支持，
   比只看版本号可靠；`OBSERVER_LIST_CXX_STANDARD=auto|17|20` 可强制。
3. **`Notify()` 双轨**（其余代码两个标准共享）：

```cpp
#ifdef OBS_HAS_CPP20
  template <typename Method, typename... Args>
    requires std::invocable<Method, ObserverType*, const Args&...>
  void Notify(Method m, const Args&... args);
#else
  template <typename Method, typename... Args,
            typename = std::enable_if_t<std::is_invocable_v<...>>>
  void Notify(Method m, const Args&... args);
#endif
```

`Compact()` 同理：C++20 用 `std::erase_if`，C++17 用 `remove_if + erase` 惯用法。

## 保留的 Chromium 最新机制

| 机制 | 说明 |
|---|---|
| `live_iterators_` 侵入式链表 | 替代旧版 `notify_depth_` 计数；迭代器存活期注册 `WeakLinkNode` |
| `MarkForRemoval()` 延迟删除 | 迭代中删除只标记，最后一个迭代器析构时 `Compact()` 真正清除 |
| `ObserverListPolicy::kAll / kExistingOnly` | 通知期间新增观察者是否本轮收到（对齐新版 enum class 命名） |
| `ObserverListReentrancyPolicy` | 默认禁重入（DCHECK 拦截）+ `GetReentrantRange()` 逃生口 |
| range-for 迭代器 | 标准前向迭代器，替代 `FOR_EACH_OBSERVER` 宏（宏已在 Chromium 移除） |
| `Notify(&Observer::OnFoo, args...)` | 成员函数指针批量通知，带编译期签名检查 |
| CheckedObserver 防悬垂 | 观察者内嵌 `LifetimeToken`（`shared_ptr<atomic_bool>`），适配器持 `weak_ptr`；迭代时发现"已析构仍挂列表"→ 断言裁决 |
| `ScopedObservation` | 构造注册 / 析构注销，从根上消灭"忘记 Remove"；含 `GetSource/GetObserver/IsObservingSource` 查询接口 |
| `ScopedMultiSourceObservation` | 多源 RAII：一个观察者同时观察多个 source（对应 NotificationRegistrar 批量 Add/RemoveAll 场景），`flat_set`→`std::set` |
| `check_empty` | 析构时断言列表为空 |
| `::Unchecked` 别名 | 不继承 CheckedObserver 的旧式观察者退回裸指针 |

## EventChannel：typed EventBus（替代 NotificationService）

NotificationService（`int` 频道号 + `void*` details）在 Chromium main 已被整体删除（`base/notification/notification_service.h` 已 404）。
`event_channel.h` 提供等价的"按消息类型发布/订阅"能力，但全部强类型化：

```cpp
// 发布方：只需知道消息结构体类型
observer_list::GetChannel<GiftMsg>().Publish({7, 100});

// 订阅方：RAII 注册，析构自动退订
struct GiftObserver : observer_list::Channel<GiftMsg>::Subscriber {
  void OnEvent(const GiftMsg& msg) override { /* ... */ }
};
observer_list::Subscription<GiftMsg> sub_{this};
sub_.Observe(&observer_list::GetChannel<GiftMsg>());
```

| 维度 | NotificationService | `Channel<Msg>` |
|---|---|---|
| 消息标识 | `int` 常量（枚举顺序敏感） | 消息结构体类型（编译期） |
| 载荷 | `Details<T>(void*)` 强转 | `OnEvent(const Msg&)` 签名检查 |
| 解耦方式 | 全局总线 + 集中常量表 | 每类型一条频道，双方只共享结构体定义 |
| 注销 | `NotificationRegistrar` 手动管理 | `Subscription<Msg>` RAII |
| 防悬垂 | 依赖单例存活期覆盖 | CheckedObserver CHECK-on-dangling |

## Review 修复记录

初版实现 review 后发现并修复 3 处边界正确性缺陷（均有对应 demo 场景背书）：

1. **迭代器在列表析构后使用 UAF**（[12]）：`Iter` 曾同时持有弱链接与裸指针，
   裸指针不随列表析构失效 → 所有访问改经 `WeakLinkNode` 判活，列表先析构后迭代器表现为 end
2. **`IsEqual` 只比裸指针**（[13]）：同地址 ABA 误判重复注册、迭代中移除后无法立即重注册
   → `IsEqual` 增加存活判断（排除已标记删除与悬垂槽）
3. **`ThreadChecker` 语义偏差**（[14]）：曾构造即绑定线程且 detach 后永真
   → 对齐 Chromium：构造即 detach、首次使用绑定，允许"构造在 A、使用在 B"

## 断言行为可配置（对 Chromium 的增强）

Chromium 的 `CHECK` 直接崩溃。本库经 `SetAssertHandler()` 允许安装自定义裁决：

- `AssertVerdict::kAbort` —— 等价 Chromium：直接终止进程
- `AssertVerdict::kContinue` —— 记录后继续；**悬垂观察者被"吞断言"时安全跳过该槽位**，
  不解引用已析构对象；`Compact()` 清理悬垂槽并对账 `observers_count_`，计数不漂移

demo 用 `kContinue` 无崩溃地演示悬垂检测，用 `kAbort` 语义验证重入禁令。

## 明确舍弃项（承接评估结论）

1. `raw_ptr<>` / MiraclePtr 悬垂保护（`::Unchecked` 路径退化为裸指针 = 旧版行为；默认路径不依赖它）
2. `DUMP_WILL_BE_*` 灰度机制（直接 CHECK；demo 变体强制 `OBSERVER_LIST_ENABLE_DCHECK` 在 Release 下也生效）
3. SequenceChecker（退化为 `std::thread::id` 线程粒度检查；单线程使用场景下等价）
4. DCHECK 构建下的注册栈符号化
5. `NotifyAllowReentrancyUntriaged`、`UncheckedAndDanglingUntriaged` 等 Chromium 存量治理专用别名
6. ThreadSafe 版（本工程观察者全在 UI 线程，列为后续扩展）

## 构建与验证

```powershell
cmake -S observer_list_modern -B observer_list_modern/build
cmake --build observer_list_modern/build --config Debug
ctest --test-dir observer_list_modern/build -C Debug --output-on-failure
```

- MSVC 自动附加 `/Zc:__cplusplus`（否则 `__cplusplus` 恒为 199711L，探测失真）与 `/utf-8`（消除 C4819）
- 构建两个 demo 变体：`observer_list_demo_cpp17`（强制 17，验证降级路径）、
  `observer_list_demo_cpp20`（验证 concepts 路径，编译器不支持时自动跳过）
- **验证状态（MSVC 19.44）**：双变体各 79 项自检全部通过，退出码 0，输出一致

## demo 自检覆盖（79 项）

1. 当前编译路径打印（C++20 / C++17）
2. `Notify` 基本通知 / 重复注册拦截（断言计数 +1，计数不变）
3. 回调中自注销（其余观察者照常收到，计数正确收敛）
4. 通知中新增观察者：`kAll` 本轮收到 vs `kExistingOnly` 本轮不收、下轮收到
5. `ScopedObservation` RAII + `GetSource/GetObserver/IsObservingSource` 查询接口
6. range-for 手动迭代（各观察者传不同参数）
7. 重入：默认禁重入被检测（断言 +1，嵌套迭代仍安全完成）；`GetReentrantRange()` 零断言
8. 悬垂观察者：迭代时被检测（`kContinue` 跳过悬垂槽，幸存者照常收到，`Compact` 修正计数）
9. `Unchecked` 裸指针列表（旧式观察者兼容路径）
10. `check_empty` 析构断言（干净路径）
11. `ScopedMultiSourceObservation` 多源注册/重复 Add 拦截/跨源通知互不干扰/单移除/移除未观察源静默/析构全注销
12. `Channel<Msg>` typed pub/sub：双订阅者收完整强类型载荷、频道按类型隔离、单点 `Reset()` 退订、析构自动退订、悬垂订阅者检测
13. 列表析构后迭代器使用安全（`WeakLinkNode` 判活，表现为 end，无 UAF）
14. 同地址 ABA 不误判重复注册（悬垂槽存活判断）+ 迭代中移除后立即重注册
15. `ThreadChecker` 首次使用绑定（主线程构造、工作线程使用零误报）
