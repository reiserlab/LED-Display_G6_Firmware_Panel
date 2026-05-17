#ifndef DISPLAY_H
#define DISPLAY_H
#include <pico.h>
#include "pico/util/queue.h"
#include "pattern.h"

class Display {

    public:
        Display(queue_t &display_queue);
        void initialize();
        void update();
        void show();

    protected:
        uint64_t col_pin_mask_ = 0;
        uint64_t row_pin_mask_ = 0;
        queue_t &display_queue_;
        Pattern pat_;
        bool have_pattern_ = false;     // true once a Pattern has been dequeued at least once
        bool oneshot_pending_ = false;  // S1.9: latch for Oneshot — true between dequeue and first scan
};

#endif
