// 现代版 ObserverList：复刻 Chromium 最新 base/observer_list.h 的核心逻辑，
// C++20 可用时走 concepts 路径，否则自动降级到 C++17 SFINAE 路径（语义一致）。
//
// 与旧版（notify_depth_ + 置空槽）相比的核心差异：
//   * live_iterators_ 侵入式链表登记存活迭代器，最后一个迭代器析构时才压缩
//   * 迭代中删除 -> MarkForRemoval()，结束后 Compact() 统一清除
//   * range-for 迭代 + Notify() 成员函数，替代 FOR_EACH_OBSERVER 宏
//   * 默认 CheckedObserver 防悬垂（析构未注销即断言暴露）
//   * 重入策略：默认禁止在迭代中再次迭代同一列表（kDisallow）
//
// 明确舍弃（C++17 无等价物或属 Chromium 存量治理）：
//   raw_ptr/MiraclePtr、DUMP_WILL_BE_* 灰度断言、sequence 粒度检查（降级为
//   thread 粒度）、NotifyAllowReentrancyUntriaged、注册栈符号化。
#ifndef OBSERVER_LIST_OBSERVER_LIST_H_
#define OBSERVER_LIST_OBSERVER_LIST_H_

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>

#ifdef OBS_HAS_CPP20
#include <concepts>
#endif

#include "observer_list/observer_list_internal.h"
#include "observer_list/port.h"

namespace observer_list {

// 通知范围策略
enum class ObserverListPolicy {
  kAll,          // 通知进行中新增的观察者本轮即会收到
  kExistingOnly, // 通知进行中新增的观察者下一轮才收到
};

// 重入策略
enum class ObserverListReentrancy {
  kDisallow,  // 已处于迭代中时再次迭代将触发 DCHECK（默认）
  kAllow,     // 允许重入迭代
};

template <class ObserverType,
          bool check_empty = false,
          ObserverListReentrancy reentrancy_policy =
              ObserverListReentrancy::kDisallow,
          class StorageAdapter = internal::CheckedObserverAdapter>
class ObserverList {
 public:
  // 裸指针观察者别名：无防悬垂，等价旧版行为；CheckedObserver 派生类型禁用
  //（RawObserverAdapter::Get 中 static_assert 拦截）
  using Unchecked = ObserverList<ObserverType, check_empty,
                                 reentrancy_policy, internal::RawObserverAdapter>;

  using value_type = ObserverType;

  // -------------------------------------------------------------------------
  // 迭代器：正规 forward iterator，支持 range-for。
  //   * 构造/拷贝时向 live_iterators_ 注册（栈上 WeakLinkNode）
  //   * 跳过 MarkForRemoval 的槽位
  //   * 是最后一个存活迭代器时，析构触发 Compact()
  // -------------------------------------------------------------------------
  template <bool check_reentrancy = true>
  class Iter {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = ObserverType;
    using difference_type = std::ptrdiff_t;
    using pointer = ObserverType*;
    using reference = ObserverType&;

    Iter() = default;

    explicit Iter(const ObserverList* list)
        : index_(0),
          max_index_(list->policy_ == ObserverListPolicy::kAll
                         ? std::numeric_limits<size_t>::max()
                         : list->observers_.size()) {
      // kDisallow 时重入迭代触发 DCHECK；check_reentrancy=false 用于显式
      // 允许重入的范围（GetReentrantRange / NotifyAllowReentrancy）
      OBS_DCHECK(!check_reentrancy ||
                 reentrancy_policy == ObserverListReentrancy::kAllow ||
                 list->live_iterators_.empty());
      node_.SetList(const_cast<ObserverList*>(list));
      EnsureValidIndex();
    }

    ~Iter() {
      // node_.get() 为空说明列表已先析构，不可再触碰
      if (node_.get() && node_.IsOnlyRemainingNode()) {
        node_.get()->Compact();
      }
      // node_ 析构时自动从链表摘除
    }

    Iter(const Iter& other)
        : index_(other.index_), max_index_(other.max_index_) {
      if (other.node_.get()) {
        node_.SetList(other.node_.get());
      }
    }

    Iter& operator=(const Iter& other) {
      if (this != &other) {
        if (node_.get() && node_.IsOnlyRemainingNode()) {
          node_.get()->Compact();
        }
        node_.Invalidate();
        index_ = other.index_;
        max_index_ = other.max_index_;
        if (other.node_.get()) {
          node_.SetList(other.node_.get());
        }
      }
      return *this;
    }

    Iter& operator++() {
      if (node_.get()) {
        ++index_;
        EnsureValidIndex();
      }
      return *this;
    }

    Iter operator++(int) {
      Iter it(*this);
      ++(*this);
      return it;
    }

    ObserverType* operator->() const { return GetCurrent(); }
    ObserverType& operator*() const { return *GetCurrent(); }

    bool operator==(const Iter& other) const {
      return (is_end() && other.is_end()) ||
             (node_.get() == other.node_.get() && index_ == other.index_);
    }
    bool operator!=(const Iter& other) const { return !(*this == other); }

   private:
    friend class ObserverList;

    ObserverType* GetCurrent() const {
      return StorageAdapter::template Get<ObserverType>(
          node_.get()->observers_[index_]);
    }

    void EnsureValidIndex() {
      ObserverList* list = node_.get();
      if (!list) {
        return;
      }
      const size_t max_index = std::min(max_index_, list->observers_.size());
      while (index_ < max_index &&
             list->observers_[index_].IsMarkedForRemoval()) {
        ++index_;
      }
    }

    size_t clamped_max_index() const {
      ObserverList* list = node_.get();
      return std::min(max_index_, list ? list->observers_.size() : size_t{0});
    }

    // 列表析构后弱链接被置空，迭代器表现为 end（不持有裸指针真值，无 UAF）
    bool is_end() const { return !node_.get() || index_ >= clamped_max_index(); }

    internal::WeakLinkNode<ObserverList> node_;
    size_t index_ = 0;
    size_t max_index_ = 0;
  };

  using iterator = Iter<true>;
  using const_iterator = Iter<true>;

  explicit ObserverList(ObserverListPolicy policy = ObserverListPolicy::kAll)
      : policy_(policy) {}

  ObserverList(const ObserverList&) = delete;
  ObserverList& operator=(const ObserverList&) = delete;

  ~ObserverList() {
    // 使所有存活迭代器失效（节点自身幂等，直接循环置空）
    while (!live_iterators_.empty()) {
      live_iterators_.head()->Invalidate();
    }
    if (check_empty) {  // if constexpr：C++17 起合法，非 check_empty 实例不编译此体
      Compact();
      OBS_ASSERT_MESSAGE(observers_.empty(),
                         "ObserverList destroyed with observers still "
                         "registered");
    }
  }

  // -------------------------------------------------------------------------
  // 注册管理
  // -------------------------------------------------------------------------

  // 前置条件：observer 非空、未注册过。重复注册默认 abort，自定义 handler
  // 裁决 kContinue 时忽略本次添加（与 Chromium 行为一致）
  void AddObserver(ObserverType* observer) {
    OBS_DCHECK(thread_checker_.CalledOnValidThread());
    if (HasObserver(observer)) {
      internal::HandleAssert(__FILE__, __LINE__,
                             "observer added twice",
                             "AddObserver called twice for the same observer");
      return;
    }
    observers_.emplace_back(StorageAdapter(observer));
    ++observers_count_;
  }

  // 未注册时无操作。迭代中调用：标记延迟删除，迭代结束后由 Compact() 清除
  void RemoveObserver(const ObserverType* observer) {
    OBS_DCHECK(thread_checker_.CalledOnValidThread());
    auto it = std::find_if(observers_.begin(), observers_.end(),
                           [observer](const StorageAdapter& a) {
                             return a.IsEqual(observer);
                           });
    if (it == observers_.end()) {
      return;
    }
    if (!it->IsMarkedForRemoval()) {
      --observers_count_;
    }
    if (live_iterators_.empty()) {
      observers_.erase(it);
    } else {
      it->MarkForRemoval();
    }
  }

  bool HasObserver(const ObserverType* observer) const {
    if (observer == nullptr) {
      return false;
    }
    return std::find_if(observers_.begin(), observers_.end(),
                        [observer](const StorageAdapter& a) {
                          return a.IsEqual(observer);
                        }) != observers_.end();
  }

  // 迭代中调用时全部标记延迟删除
  void Clear() {
    if (live_iterators_.empty()) {
      observers_.clear();
    } else {
      for (auto& adapter : observers_) {
        adapter.MarkForRemoval();
      }
    }
    observers_count_ = 0;
  }

  size_t size() const { return observers_count_; }
  bool empty() const { return observers_count_ == 0; }
  bool might_have_observers() const { return !empty(); }

  // -------------------------------------------------------------------------
  // 迭代入口
  // -------------------------------------------------------------------------

  const_iterator begin() const {
    OBS_DCHECK(thread_checker_.CalledOnValidThread());
    if (observers_count_ == 0) {
      return const_iterator();  // 空列表不注册弱链接（快速路径）
    }
    return const_iterator(this);
  }

  const_iterator end() const { return const_iterator(); }

  // 显式允许重入的范围适配器（kDisallow 列表的合法重入口）
  struct ReentrantRange {
    Iter<false> begin_iter;
    Iter<false> end_iter;
    Iter<false> begin() const { return begin_iter; }
    Iter<false> end() const { return end_iter; }
  };

  ReentrantRange GetReentrantRange() {
    if (observers_count_ == 0) {
      return ReentrantRange{};
    }
    return ReentrantRange{Iter<false>(this), Iter<false>()};
  }

  // -------------------------------------------------------------------------
  // 通知：C++20 用 requires 约束，C++17 用 enable_if 约束，语义一致
  // -------------------------------------------------------------------------

#ifdef OBS_HAS_CPP20
  template <typename Method, typename... Args>
    requires std::invocable<Method, ObserverType*, const Args&...>
  void Notify(Method method, const Args&... args) {
    NotifyImpl(method, args...);
  }

  template <typename Method, typename... Args>
    requires std::invocable<Method, ObserverType*, const Args&...>
  void NotifyAllowReentrancy(Method method, const Args&... args) {
    NotifyReentrantImpl(method, args...);
  }
#else
  template <typename Method, typename... Args,
            typename = std::enable_if_t<
                std::is_invocable_v<Method, ObserverType*, const Args&...>>>
  void Notify(Method method, const Args&... args) {
    NotifyImpl(method, args...);
  }

  template <typename Method, typename... Args,
            typename = std::enable_if_t<
                std::is_invocable_v<Method, ObserverType*, const Args&...>>>
  void NotifyAllowReentrancy(Method method, const Args&... args) {
    NotifyReentrantImpl(method, args...);
  }
#endif

 private:
  friend class internal::WeakLinkNode<ObserverList>;

  template <typename Method, typename... Args>
  void NotifyImpl(Method method, const Args&... args) {
    for (auto& observer : *this) {
      std::invoke(method, observer, args...);
    }
  }

  template <typename Method, typename... Args>
  void NotifyReentrantImpl(Method method, const Args&... args) {
    for (auto& observer : GetReentrantRange()) {
      std::invoke(method, observer, args...);
    }
  }

  // 压缩：清除所有标记删除的槽位。C++20 用 std::erase_if，C++17 用惯用法。
  // 悬垂观察者（kContinue 被吞断言时）在此被 IsMarkedForRemoval 自动标记清除。
  // 结束后对账计数，修正容错路径下的漂移。
  void Compact() {
    auto is_removed = [](const StorageAdapter& a) {
      return a.IsMarkedForRemoval();
    };
#ifdef OBS_HAS_CPP20
    std::erase_if(observers_, is_removed);
#else
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(), is_removed),
        observers_.end());
#endif
    observers_count_ = observers_.size();
  }

  std::vector<StorageAdapter> observers_;
  internal::IntrusiveList<internal::WeakLinkNode<ObserverList>> live_iterators_;
  size_t observers_count_ = 0;
  const ObserverListPolicy policy_;
  internal::ThreadChecker thread_checker_;
};

// 重入允许列表的便捷别名（对应 Chromium ReentrantObserverList）
template <class ObserverType, bool check_empty = false>
using ReentrantObserverList =
    ObserverList<ObserverType, check_empty, ObserverListReentrancy::kAllow>;

}  // namespace observer_list

#endif  // OBSERVER_LIST_OBSERVER_LIST_H_
