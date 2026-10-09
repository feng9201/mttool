// 防悬垂观察者基类（对应 Chromium base::CheckedObserver）。
// 观察者析构后若仍注册在任一 ObserverList，下一次迭代/压缩将通过断言暴露，
// 默认 abort；安装 kContinue 的 handler 时该槽位被安全跳过（不解引用死对象）。
#ifndef OBSERVER_LIST_OBSERVER_LIST_TYPES_H_
#define OBSERVER_LIST_OBSERVER_LIST_TYPES_H_

#include <atomic>
#include <memory>

#include "observer_list/port.h"

namespace observer_list {

namespace internal {
class CheckedObserverAdapter;
}  // namespace internal

class CheckedObserver {
 public:
  CheckedObserver() : lifetime_(std::make_shared<Lifetime>()) {}

  virtual ~CheckedObserver() {
    lifetime_->alive.store(false, std::memory_order_release);
  }

  CheckedObserver(const CheckedObserver&) = delete;
  CheckedObserver& operator=(const CheckedObserver&) = delete;

 private:
  friend class internal::CheckedObserverAdapter;

  struct Lifetime {
    std::atomic_bool alive{true};
  };

  std::shared_ptr<Lifetime> lifetime_;
};

}  // namespace observer_list

#endif  // OBSERVER_LIST_OBSERVER_LIST_TYPES_H_
