
#ifndef UTIL_TIMETICKER_H_
#define UTIL_TIMETICKER_H_
/**
*@brief 此对象可以用于代码执行时间统计，以可以用于一般计时
* auto itime = tick.elapsedTime();
	tick.resetTime();
	std::cout << "times: " << itime << std::endl;
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	itime = tick.elapsedTime();
	std::cout << "next times: " << itime << std::endl;
*/

#include <cassert>
#include <chrono>
#include <ctime>

namespace mttool {

class Ticker {
public:
    Ticker(uint64_t min_ms = 0){
        _created = _begin = getCurrentMillisecond();
        _min_ms = min_ms;
    }

    ~Ticker() {
    }

    uint64_t getCurrentMillisecond() const {
        auto now = std::chrono::system_clock::now();
        // 转换为时间戳（从 1970-01-01 00:00:00 UTC 开始的毫秒数）
        auto duration = now.time_since_epoch();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
        return milliseconds.count();
    }

    /**
     * 获取上次resetTime后至今的时间，单位毫秒
     */
    uint64_t elapsedTime() const {
        return getCurrentMillisecond() - _begin;
    }

    /**
     * 获取从创建至今的时间，单位毫秒
     */
    uint64_t createdTime() const {
        return getCurrentMillisecond() - _created;
    }

    /**
     * 重置计时器
     */
    void resetTime() {
        _only_create = false;
        _begin = getCurrentMillisecond();
    }

    /**
     * 重置开始时间
     */
    void resetCrateTime() {
        _only_create = false;
        _created = _begin = getCurrentMillisecond();
    }

    /**
    *  仅初始化未重置计时器
    */
    bool isOnlyCreate() {
        return _only_create;
    }
private:
    uint64_t _min_ms;
    uint64_t _begin;
    uint64_t _created;
    bool     _only_create = true;
};

class SmoothTicker {
public:
    /**
     * 此对象用于生成平滑的时间戳
     * @param reset_ms 时间戳重置间隔，没间隔reset_ms毫秒, 生成的时间戳会同步一次系统时间戳
     */
    SmoothTicker(uint64_t reset_ms = 10000) {
        _reset_ms = reset_ms;
        _ticker.resetTime();
    }

    ~SmoothTicker() {}

    /**
     * 返回平滑的时间戳，防止由于网络抖动导致时间戳不平滑
     */
    uint64_t elapsedTime() {
        auto now_time = _ticker.elapsedTime();
        if (_first_time == 0) {
            if (now_time < _last_time) {
                auto last_time = _last_time - _time_inc;
                double elapse_time = (now_time - last_time);
                _time_inc += (elapse_time / ++_pkt_count) / 3;
                auto ret_time = last_time + _time_inc;
                _last_time = (uint64_t) ret_time;
                return (uint64_t) ret_time;
            }
            _first_time = now_time;
            _last_time = now_time;
            _pkt_count = 0;
            _time_inc = 0;
            return now_time;
        }

        auto elapse_time = (now_time - _first_time);
        _time_inc += elapse_time / ++_pkt_count;
        auto ret_time = _first_time + _time_inc;
        if (elapse_time > _reset_ms) {
            _first_time = 0;
        }
        _last_time = (uint64_t) ret_time;
        return (uint64_t) ret_time;
    }

    /**
     * 时间戳重置为0开始
     */
    void resetTime() {
        _first_time = 0;
        _pkt_count = 0;
        _ticker.resetTime();
    }

private:
    double _time_inc = 0;
    uint64_t _first_time = 0;
    uint64_t _last_time = 0;
    uint64_t _pkt_count = 0;
    uint64_t _reset_ms;
    Ticker _ticker;
};
} 
#endif 