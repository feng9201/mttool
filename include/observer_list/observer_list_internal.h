// 内部设施：侵入式链表（live_iterators_ 注册）、栈上弱链接节点、存储适配器、
// 线程绑定检查。对应 Chromium base/observer_list_internal.h 的 C++17 可移植复刻。
#ifndef OBSERVER_LIST_OBSERVER_LIST_INTERNAL_H_
#define OBSERVER_LIST_OBSERVER_LIST_INTERNAL_H_

#include <thread>
#include <type_traits>
#include <utility>

#include "observer_list/observer_list_types.h"
#include "observer_list/port.h"

namespace observer_list {
namespace internal {

// ---------------------------------------------------------------------------
// 侵入式双向链表：节点自带前后指针，WeakLinkNode 挂在迭代器栈帧上，零分配。
// 仅服务 ObserverList::live_iterators_，不做通用容器。
// ---------------------------------------------------------------------------
template <class Node>
class IntrusiveList {
 public:
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  Node* head() const { return head_; }

  void Append(Node* node) {
    node->prev_ = tail_;
    node->next_ = nullptr;
    if (tail_) {
      tail_->next_ = node;
    } else {
      head_ = node;
    }
    tail_ = node;
    ++size_;
  }

  void Remove(Node* node) {
    if (node->prev_) {
      node->prev_->next_ = node->next_;
    } else {
      head_ = node->next_;
    }
    if (node->next_) {
      node->next_->prev_ = node->prev_;
    } else {
      tail_ = node->prev_;
    }
    node->prev_ = nullptr;
    node->next_ = nullptr;
    --size_;
  }

 private:
  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  size_t size_ = 0;
};

// 观察者列表的"轻量弱指针"：替代旧版引用计数式 base::WeakPtr。
// 节点随迭代器析构自动从链表摘除；列表析构时将所有节点置空。
template <class OwnerList>
class WeakLinkNode {
 public:
  WeakLinkNode() = default;
  ~WeakLinkNode() { Invalidate(); }

  WeakLinkNode(const WeakLinkNode&) = delete;
  WeakLinkNode& operator=(const WeakLinkNode&) = delete;

  void SetList(OwnerList* list) {
    list_ = list;
    list->live_iterators_.Append(this);
  }

  // 从链表摘除并置空；幂等，重复调用安全
  void Invalidate() {
    if (list_) {
      OwnerList* list = list_;
      list_ = nullptr;
      list->live_iterators_.Remove(this);
    }
  }

  bool IsOnlyRemainingNode() const {
    return list_ && list_->live_iterators_.size() == 1;
  }

  OwnerList* get() const { return list_; }
  OwnerList* operator->() const { return list_; }
  explicit operator bool() const { return list_ != nullptr; }

 private:
  friend class IntrusiveList<WeakLinkNode>;
  friend OwnerList;

  OwnerList* list_ = nullptr;
  WeakLinkNode* prev_ = nullptr;
  WeakLinkNode* next_ = nullptr;
};

// ---------------------------------------------------------------------------
// 存储适配器：统一 MarkForRemoval / IsMarkedForRemoval / IsEqual / Get 接口
// ---------------------------------------------------------------------------

// 裸指针版（ObserverList<T>::Unchecked）：无防悬垂检测，等价旧版行为
class RawObserverAdapter {
 public:
  explicit RawObserverAdapter(const void* observer)
      : observer_(const_cast<void*>(observer)) {}

  RawObserverAdapter(const RawObserverAdapter&) = delete;
  RawObserverAdapter& operator=(const RawObserverAdapter&) = delete;
  RawObserverAdapter(RawObserverAdapter&&) = default;
  RawObserverAdapter& operator=(RawObserverAdapter&&) = default;

  void MarkForRemoval() { observer_ = nullptr; }
  bool IsMarkedForRemoval() const { return observer_ == nullptr; }
  bool IsEqual(const void* rhs) const { return observer_ == rhs; }

  template <class ObserverType>
  static ObserverType* Get(const RawObserverAdapter& adapter) {
    static_assert(!std::is_base_of<CheckedObserver, ObserverType>::value,
                  "CheckedObserver-derived types must not use "
                  "ObserverList<T>::Unchecked");
    return static_cast<ObserverType*>(adapter.observer_);
  }

 private:
  void* observer_;
};

// Checked 版（默认）：包 weak 引用观察者生命周期，迭代路径上暴露悬垂
class CheckedObserverAdapter {
 public:
  explicit CheckedObserverAdapter(const CheckedObserver* observer)
      : lifetime_(observer->lifetime_),
        observer_(const_cast<CheckedObserver*>(observer)) {}

  CheckedObserverAdapter(const CheckedObserverAdapter&) = delete;
  CheckedObserverAdapter& operator=(const CheckedObserverAdapter&) = delete;
  CheckedObserverAdapter(CheckedObserverAdapter&&) = default;
  CheckedObserverAdapter& operator=(CheckedObserverAdapter&&) = default;

  void MarkForRemoval() { marked_ = true; }

  bool IsMarkedForRemoval() const {
    if (!marked_) {
      auto life = lifetime_.lock();
      if (!life || !life->alive.load(std::memory_order_acquire)) {
        // 观察者已析构却未注销：默认 abort；handler 裁决 kContinue 时，
        // 将其视为已移除，迭代器会安全跳过本槽（绝不解引用死对象）
        HandleAssert(__FILE__, __LINE__,
                     "observer destroyed while still registered",
                     "CheckedObserver was destructed but never removed "
                     "from an ObserverList");
        marked_ = true;
      }
    }
    return marked_;
  }

  bool IsEqual(const CheckedObserver* rhs) const {
    // 排除已标记删除与悬垂槽：同地址 ABA 不会误判，迭代中移除后可立即重注册
    if (marked_) {
      return false;
    }
    auto life = lifetime_.lock();
    return life && life->alive.load(std::memory_order_acquire) &&
           observer_ == rhs;
  }

  template <class ObserverType>
  static ObserverType* Get(const CheckedObserverAdapter& adapter) {
    static_assert(std::is_base_of<CheckedObserver, ObserverType>::value,
                  "ObserverType must derive from CheckedObserver, "
                  "or use ObserverList<T>::Unchecked");
    OBS_CHECK(!adapter.IsMarkedForRemoval());
    return static_cast<ObserverType*>(adapter.observer_);
  }

 private:
  std::weak_ptr<CheckedObserver::Lifetime> lifetime_;
  CheckedObserver* observer_;
  mutable bool marked_ = false;
};

// ---------------------------------------------------------------------------
// 轻量线程绑定检查（SEQUENCE_CHECKER 的降级实现：sequence -> thread 粒度）
// ---------------------------------------------------------------------------
class ThreadChecker {
 public:
  // 构造即 detach，首次使用时绑定当前线程（对齐 Chromium 的
  // DETACH_FROM_SEQUENCE 语义，允许"构造在线程 A、全程使用在线程 B"）
  ThreadChecker() = default;

  void DetachFromThread() { thread_id_ = std::thread::id(); }

  bool CalledOnValidThread() const {
    if (thread_id_ == std::thread::id()) {
      thread_id_ = std::this_thread::get_id();  // 首次使用绑定
    }
    return thread_id_ == std::this_thread::get_id();
  }

 private:
  mutable std::thread::id thread_id_;
};

}  // namespace internal
}  // namespace observer_list

#endif  // OBSERVER_LIST_OBSERVER_LIST_INTERNAL_H_
