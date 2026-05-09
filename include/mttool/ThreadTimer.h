#ifndef _THREAD_TIMER__H_
#define _THREAD_TIMER__H_

#include <assert.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

/*
@brief: 基于c++11线程实现的定时器 
example

ThreadTimer ttm;
std::cout << "start thread \n" << std::endl;
ttm.startThread(400);

// test delay timer
std::cout << "start delay timer 5 2 \n" << std::endl;
int timer1 = ttm.startTimer(5000, 2000, [](void*) {
        std::cout << "delay timer 5 2 \n" << std::endl;

});

// test normal timer
std::cout << "start normal timer 0 1 \n" << std::endl;
int timer2 = ttm.startTimer(0, 1000, [](void*) {
        std::cout << "normal timer 0 1 \n" << std::endl;
});

std::this_thread::sleep_for(std::chrono::seconds(20));


ttm.stopTimer(timer1);
std::cout << "stop delay timer 5 2 \n" << std::endl;

std::this_thread::sleep_for(std::chrono::seconds(5));

ttm.stopTimer(timer2);
std::cout << "stop normal timer 0 1 \n" << std::endl;

ttm.stopThread();
std::cout << "stop thread \n" << std::endl;

std::cout << "success \n" << std::endl;
*/

namespace mttool{
  // Conversion factor from 1ms to 1us.
  #define MICROSECONDS_PER_MILLISECOND 1000

  /**
   *@brief 单线程定时器，支持多定时器
  *		 定时器不可做耗时任务
  */
  class ThreadTimer {
  public:
    enum {
      INVALID_TIMER_ID = -1,
    };

  public:
    ThreadTimer() {
    }
    ~ThreadTimer() {
    }

    /**
     * @brief 启动定时器线程
     * @param [in] tict 单位ms
     *         定时触发周期，决定定时器最小精度
     */
    void startThread(int tick, std::function<void()> onThreadStart = nullptr,
                    std::function<void()> onThreadStop = nullptr) {
      if (m_timerThread) {
        return;
      }
      m_quitFlag = false;
      m_onThreadStart = onThreadStart;
      m_onThreadStop = onThreadStop;
      if (tick <= 0) {
        tick = 500;
      }
      m_tick = std::chrono::microseconds(tick * MICROSECONDS_PER_MILLISECOND);
      m_timerThread = new std::thread(&ThreadTimer::threadProc, this);
    }

    /**
     * @brief 停止定时器线程(不能在定时器回调中调用)
     */
    void stopThread() {
      if (!m_timerThread) {
        return;
      }

      m_quitFlag = true;

      {
        std::unique_lock<std::mutex> lock(m_waitCondMutex);
        m_waitCond.notify_one();
      }

      m_timerThread->join();
      delete m_timerThread;
      m_timerThread = nullptr;
    }

    /**
     * @brief reset tick after startThread
     * @param [in] tict ms
     */
    void setTick(int tick) {
      if (tick <= 0) {
        tick = 500;
      }
      m_tick = std::chrono::microseconds(tick * MICROSECONDS_PER_MILLISECOND);
    }

    bool IsThreadRuning() const {
      return m_timerThread;
    }

    /**
     * @brief 启动定时器
     * @param [in] delay
     *         延时启动时间，单位ms
     * @param [in] interval
     *         定时器触发间隔，单位ms
     * @param [in] cb
     *         定时器回调函数
     * @param [in] params
     *         定时器回调函数参数
     * @param [in] bSingleShot
     *         是否单次触发
     * @return
     *        -1: 失败
     *     other: 定时器id
     */
    int startTimer(int delay, int interval,  std::function<void(void*)> cb, 
      void* params = nullptr, bool bSingleShot = false) {
      if (!m_timerThread || delay < 0 || interval <= 0 || !cb) {
        return INVALID_TIMER_ID;
      }
      std::unique_lock<std::mutex> lock(m_waitCondMutex);
      int timerID = m_timerID++;
      if (m_timerID >= 999999) {
        assert(0);
        return INVALID_TIMER_ID;
      }
      TimerNode node(timerID, delay, interval, cb, params);
      node.m_singleShot = bSingleShot;

      m_timers.push_back(node);
      return timerID;
    }
    
    void singleShot(int interval, std::function<void(void*)> cb, void* params = nullptr) {
      startTimer(0, interval, cb, params, true); 
    }

    /**
     * @brief 停止定时器
     * @param [in] id
     *         定时器id
     */
    void stopTimer(int id) {
      if (!m_timerThread || id < 0 || id > 999999) {
        return;
      }
      if (std::this_thread::get_id() == m_timerThread->get_id()) {
        std::unique_lock<std::mutex> lock(m_waitCondMutex);
        std::vector<TimerNode>::iterator it = m_timers.begin();
        for (; it != m_timers.end(); it++) {
          if (it->m_id == id) {
            it->m_deleteLater = true;
            break;
          }
        }
      } else {
        std::unique_lock<std::mutex> lockOut(m_outTimersMutex);
        std::unique_lock<std::mutex> lock(m_waitCondMutex);
        std::vector<TimerNode>::iterator it = m_timers.begin();
        for (; it != m_timers.end(); it++) {
          if (it->m_id == id) {
            it->m_deleteLater = true;
            break;
          }
        }
      }
    }

    /**
     * @brief 停止定时器
     * @param [in] id
     *         定时器id
     */
    void stopAllTimer() {
      if (!m_timerThread) {
        return;
      }
      if (std::this_thread::get_id() == m_timerThread->get_id()) {
        std::unique_lock<std::mutex> lock(m_waitCondMutex);
        std::vector<TimerNode>::iterator it = m_timers.begin();
        for (; it != m_timers.end(); it++) {
          it->m_deleteLater = true;
          break;
        }
      } else {
        std::unique_lock<std::mutex> lockOut(m_outTimersMutex);
        std::unique_lock<std::mutex> lock(m_waitCondMutex);
        std::vector<TimerNode>::iterator it = m_timers.begin();
        for (; it != m_timers.end(); it++) {
          it->m_deleteLater = true;
          break;
        }
      }
    }

  protected:
    class TimerNode {
    public:
      TimerNode(int id, int delay, int interval, std::function<void(void*)> cb, void* params = nullptr)
          : m_interval(std::chrono::microseconds(interval * MICROSECONDS_PER_MILLISECOND)),
          m_id(id), m_cb(cb), m_userData(params) {
        if (delay == 0) {
          m_expire = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() +
                                                                            m_interval);
        } else {
          m_expire = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() +
                                                                            std::chrono::microseconds(delay * MICROSECONDS_PER_MILLISECOND));
        }
      }

    public:
      std::chrono::time_point<std::chrono::steady_clock, std::chrono::microseconds> m_expire;
      std::chrono::microseconds m_interval = std::chrono::microseconds(0);
      int m_id;
      std::function<void(void*)> m_cb = nullptr;
      void* m_userData = nullptr;
      bool m_deleteLater = false;
      bool m_singleShot = false;
    };

    void threadProc() {
      if (m_onThreadStart) {
        m_onThreadStart();
      }
      while (!m_quitFlag) {
        tick();
        {
          std::unique_lock<std::mutex> lock(m_waitCondMutex);
          std::chrono::microseconds realTick = m_tick - (m_consume % m_tick);
          m_waitCond.wait_for(lock, realTick);
        }
      }
      m_timers.clear();
      if (m_onThreadStop) {
        m_onThreadStop();
      }
    }

  private:
    void tick() {
      std::chrono::time_point<std::chrono::steady_clock, std::chrono::microseconds> curStart;
      curStart = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

      std::vector<TimerNode*> outTimers;
      // delete invaild timer, get timeout timer
      {
        std::unique_lock<std::mutex> lock(m_waitCondMutex);
        auto it = m_timers.begin();
        while (it != m_timers.end()) {
          if (it->m_deleteLater) {
            it = m_timers.erase(it);
          } else {
            if (curStart >= it->m_expire) {
              outTimers.push_back(&(*it));
            }
            it++;
          }
        }
      }

      // run out timer
      {
        std::unique_lock<std::mutex> lock(m_outTimersMutex);
        for (auto& item : outTimers) {
          if (!item->m_deleteLater && item->m_cb) {
            item->m_cb(item->m_userData);
            item->m_expire = curStart + item->m_interval;
          }
          if(item->m_singleShot) {
            stopTimer(item->m_id);
          }
        }
      }

      std::chrono::time_point<std::chrono::steady_clock, std::chrono::microseconds> curStop;
      curStop = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());
      m_consume = curStop - curStart;
    }

  private:
    std::vector<TimerNode> m_timers;
    std::mutex m_outTimersMutex;
    std::mutex m_waitCondMutex;
    std::condition_variable m_waitCond;
    std::atomic<bool> m_quitFlag = {false};
    std::thread* m_timerThread = nullptr;
    std::chrono::microseconds m_tick;
    std::chrono::microseconds m_consume = std::chrono::microseconds(0);
    int m_timerID = 0;
    std::function<void()> m_onThreadStart = nullptr;
    std::function<void()> m_onThreadStop = nullptr;
  };
}
#endif  //_THREAD_TIMER__H_
