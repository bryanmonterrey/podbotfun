#pragma once

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "eyes/types.hpp"

namespace eyes {

struct ImuPacket {
    MotionSample sample{};
};

class ImuService {
  public:
    ImuService() = default;
    ~ImuService() = default;
    ImuService(const ImuService &) = delete;
    ImuService &operator=(const ImuService &) = delete;

    bool start(QueueHandle_t motion_queue, QueueHandle_t calibration_queue,
               Calibration calibration, bool auto_calibrate);
    void request_calibration() { calibration_requested_.store(true); }

  private:
    static void task_entry(void *argument);
    void run();

    QueueHandle_t motion_queue_{nullptr};
    QueueHandle_t calibration_queue_{nullptr};
    Calibration calibration_{};
    std::atomic<bool> calibration_requested_{false};
};

}  // namespace eyes
