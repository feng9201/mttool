// 现代版 ObserverList 的可移植基础设施：C++ 标准探测 + 可配置断言。
#ifndef OBSERVER_LIST_PORT_H_
#define OBSERVER_LIST_PORT_H_

#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// C++ 标准探测。MSVC 必须加 /Zc:__cplusplus，否则 __cplusplus 固定报 199711L
//（CMakeLists 中已为 INTERFACE 库统一附加）。
// 定义 OBSERVER_LIST_DISABLE_CPP20 可强制编译器支持 C++20 时也走降级路径。
// ---------------------------------------------------------------------------
#if defined(_MSVC_LANG)
#  define OBS_CXX_LANG _MSVC_LANG
#else
#  define OBS_CXX_LANG __cplusplus
#endif

#if OBS_CXX_LANG >= 202002L && !defined(OBSERVER_LIST_DISABLE_CPP20)
#  define OBS_HAS_CPP20 1
#endif

namespace observer_list {

// 断言失败时自定义 handler 的裁决结果。
enum class AssertAction {
  kAbort,     // 报告后终止进程（默认行为）
  kContinue,  // 报告后继续执行；悬垂观察者等场景由调用方保证安全降级（跳过槽位）
};

using AssertHandler = AssertAction (*)(const char* file, int line,
                                       const char* condition,
                                       const char* message);

namespace internal {

// 函数局部 static + inline：header-only 下跨编译单元唯一（C++17 保证）
inline AssertHandler& AssertHandlerSlot() {
  static AssertHandler handler = nullptr;
  return handler;
}

inline AssertHandler GetAssertHandler() { return AssertHandlerSlot(); }

// 安装 handler 并返回旧值；传 nullptr 恢复默认（打印后 abort）
inline AssertHandler SetAssertHandler(AssertHandler handler) {
  AssertHandler& slot = AssertHandlerSlot();
  AssertHandler old = slot;
  slot = handler;
  return old;
}

// 断言统一入口。
inline void HandleAssert(const char* file, int line, const char* condition,
                         const char* message) {
  AssertHandler handler = AssertHandlerSlot();
  if (handler) {
    if (handler(file, line, condition, message) == AssertAction::kContinue) {
      return;
    }
  } else {
    std::fprintf(stderr,
                 "[observer_list] CHECK failed: %s  (%s:%d)%s%s\n",
                 condition, file, line, message ? "  " : "",
                 message ? message : "");
  }
  std::abort();
}

}  // namespace internal

// 以下为公开 API：安装/查询断言 handler。
// 注意：非线程安全，应在程序启动期配置一次。

inline AssertHandler SetAssertHandler(AssertHandler handler) {
  return internal::SetAssertHandler(handler);
}

inline AssertHandler GetAssertHandler() {
  return internal::GetAssertHandler();
}

}  // namespace observer_list

#define OBS_ASSERT_MESSAGE(cond, msg)                    \
  do {                                                   \
    if (!(cond)) {                                       \
      ::observer_list::internal::HandleAssert(           \
          __FILE__, __LINE__, #cond, msg);               \
    }                                                    \
  } while (0)

#define OBS_CHECK(cond) OBS_ASSERT_MESSAGE(cond, nullptr)

// DCHECK：Debug 构建，或显式定义 OBSERVER_LIST_ENABLE_DCHECK 时生效
#if !defined(NDEBUG) || defined(OBSERVER_LIST_ENABLE_DCHECK)
#  define OBS_DCHECK(cond) OBS_ASSERT_MESSAGE(cond, nullptr)
#else
#  define OBS_DCHECK(cond) ((void)0)
#endif

#endif  // OBSERVER_LIST_PORT_H_
