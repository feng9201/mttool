#ifndef UTIL_ONCETOKEN_H_
#define UTIL_ONCETOKEN_H_
/***********
@brief:仅执行一次的方法,支持初始化时执行单独任务，结束时执行单独任务
@date:25-3-28
@使用方式
1.全局初始化一些需要执行过程的内容
static onceToken token([=] {
    std::cout << "mttool start...." << std::endl;
});
2.离开作用域需要执行的一些操作
{
        onceToken token(nullptr,[=] {
            std::cout << "oncet START...." << std::endl;
        });
        std::cout << "sleep start...." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::cout << "sleep end...." << std::endl;
}
{
        onceToken token([=] {
            std::cout << "oncet START...." << std::endl;
        },
        [=] {
            std::cout << "oncet end...." << std::endl;
        });
        std::cout << "sleep start...." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::cout << "sleep end...." << std::endl;
    }
*************/

#include <functional>
#include <type_traits>

namespace mttool {

class onceToken {
public:
    using task = std::function<void(void)>;

    template<typename FUNC>
    onceToken(const FUNC &onConstructed, task onDestructed = nullptr) {
        onConstructed();
        _onDestructed = std::move(onDestructed);
    }

    onceToken(std::nullptr_t, task onDestructed = nullptr) {
        _onDestructed = std::move(onDestructed);
    }

    ~onceToken() {
        if (_onDestructed) {
            _onDestructed();
        }
    }

private:
    onceToken() = delete;
    onceToken(const onceToken &) = delete;
    onceToken(onceToken &&) = delete;
    onceToken &operator=(const onceToken &) = delete;
    onceToken &operator=(onceToken &&) = delete;

private:
    task _onDestructed;
};
}
#endif 
