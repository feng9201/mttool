// RAII 观察注册（对应 Chromium base::ScopedObservation / base::ScopedMultiSourceObservation）：
// Observe()/AddObservation() 时注册，析构或 Reset()/RemoveAll() 时自动注销，
// 从根上消灭"忘记 RemoveObserver"。
#ifndef OBSERVER_LIST_SCOPED_OBSERVATION_H_
#define OBSERVER_LIST_SCOPED_OBSERVATION_H_

#include <set>
#include <utility>

#include "observer_list/port.h"

namespace observer_list {

// 单源版：一次只观察一个 source。
// Source 需提供 AddObserver(Observer*) / RemoveObserver(const Observer*)。
template <typename Source, typename Observer>
class ScopedObservation {
 public:
  explicit ScopedObservation(Observer* observer) : observer_(observer) {}

  ScopedObservation(const ScopedObservation&) = delete;
  ScopedObservation& operator=(const ScopedObservation&) = delete;

  ~ScopedObservation() { Reset(); }

  // 比主分支宽松：主分支 DCHECK 要求未在观察，这里自动注销旧源再切换。
  void Observe(Source* source) {
    Reset();
    source_ = source;
    source_->AddObserver(observer_);
  }

  // 注销当前观察；未在观察时无操作。
  void Reset() {
    if (source_) {
      Source* source = std::exchange(source_, nullptr);
      source->RemoveObserver(observer_);
    }
  }

  // Returns true if any source is being observed.
  bool IsObserving() const { return source_ != nullptr; }

  // Returns true if |source| is the observed source.
  bool IsObservingSource(const Source* source) const { return source_ == source; }

  Observer* GetObserver() { return observer_; }
  const Observer* GetObserver() const { return observer_; }

  Source* GetSource() { return source_; }
  const Source* GetSource() const { return source_; }

 private:
  Observer* observer_;
  Source* source_ = nullptr;
};

// 多源版：同一观察者同时观察多个 source，
// 对应旧 NotificationRegistrar 批量 Add / RemoveAll 的场景。
// raw_ptr/flat_set 在 C++17 下分别降级为裸指针与 std::set。
template <typename Source, typename Observer>
class ScopedMultiSourceObservation {
 public:
  using Sources = std::set<Source*>;

  explicit ScopedMultiSourceObservation(Observer* observer) : observer_(observer) {}

  ScopedMultiSourceObservation(const ScopedMultiSourceObservation&) = delete;
  ScopedMultiSourceObservation& operator=(const ScopedMultiSourceObservation&) = delete;

  ~ScopedMultiSourceObservation() { RemoveAll(); }

  void AddObservation(Source* source) {
    OBS_DCHECK(source != nullptr);
    if (sources_.find(source) != sources_.end()) {
      // 主分支语义：重复 AddObservation 属调用方 bug；忽略以保证注销一一对应
      OBS_DCHECK(false && "duplicate AddObservation");
      return;
    }
    sources_.insert(source);
    source->AddObserver(observer_);
  }

  // 移除对 |source| 的观察；未在观察该源时为无操作（不触发断言）。
  void RemoveObservation(Source* source) {
    if (sources_.erase(source) != 0) {
      source->RemoveObserver(observer_);
    }
  }

  void RemoveAll() {
    for (Source* source : sources_) {
      source->RemoveObserver(observer_);
    }
    sources_.clear();
  }

  bool IsObservingSource(const Source* source) const {
    return sources_.find(const_cast<Source*>(source)) != sources_.end();
  }

  size_t size() const { return sources_.size(); }
  const Sources& GetSourcesForTest() const { return sources_; }

  Observer* GetObserver() { return observer_; }
  const Observer* GetObserver() const { return observer_; }

 private:
  Observer* observer_;
  Sources sources_;
};

}  // namespace observer_list

#endif  // OBSERVER_LIST_SCOPED_OBSERVATION_H_
