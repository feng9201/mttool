// typed EventBus 频道（方案 2）：显式声明"发布/订阅的消息类型"，
// 替代被 Chromium 删除的 NotificationService（int 频道号 + void* details）。
//
// 发布方与订阅方只需共享消息结构体定义，彼此零类型依赖：
//   发布：observer_list::GetChannel<GiftMsg>().Publish(msg);
//   订阅：observer_list::Subscription<GiftMsg> sub{this};
//         sub.Observe(&observer_list::GetChannel<GiftMsg>());
//
// 与 NotificationService 的本质区别：频道按 Msg 模板参数强类型化，
// 订阅接口方法签名即类型契约，编译期检查，无类型擦除强转。
#ifndef OBSERVER_LIST_EVENT_CHANNEL_H_
#define OBSERVER_LIST_EVENT_CHANNEL_H_

#include "observer_list/observer_list.h"
#include "observer_list/scoped_observation.h"

namespace observer_list {

// 每个 Msg 类型一条频道。与 ObserverList 同样非线程安全：
// 发布/订阅/退订须发生在同一线程（本工程为 UI 线程）。
template <typename Msg>
class Channel {
 public:
  // 订阅接口：方法签名即消息类型契约
  class Subscriber : public CheckedObserver {
   public:
    virtual ~Subscriber() = default;
    virtual void OnEvent(const Msg& msg) = 0;
  };

  void AddObserver(Subscriber* observer) { list_.AddObserver(observer); }
  void RemoveObserver(Subscriber* observer) { list_.RemoveObserver(observer); }
  bool HasObserver(Subscriber* observer) const {
    return list_.HasObserver(observer);
  }
  size_t ObserverCount() const { return list_.size(); }

  // 同步分发：Publish 返回时所有订阅者均已处理完毕（kAll 语义，
  // 分发中新增订阅者本轮也会收到；迭代中退订安全）
  void Publish(const Msg& msg) { list_.Notify(&Subscriber::OnEvent, msg); }

 private:
  ObserverList<Subscriber> list_;
};

// 每个 Msg 类型进程内唯一频道（magic static，C++11 起构造线程安全）。
// 注意：频道本身非线程安全，见 Channel 注释。
template <typename Msg>
Channel<Msg>& GetChannel() {
  static Channel<Msg> channel;
  return channel;
}

// 订阅别名：RAII，析构自动退订（替代 NotificationRegistrar）
template <typename Msg>
using Subscription =
    ScopedObservation<Channel<Msg>, typename Channel<Msg>::Subscriber>;

}  // namespace observer_list

#endif  // OBSERVER_LIST_EVENT_CHANNEL_H_
