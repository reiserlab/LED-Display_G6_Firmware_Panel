#ifndef MESSAGE_RECEIVER_H
#define MESSAGE_RECEIVER_H
#include <functional>
#include <unordered_map>
#include "pico/util/queue.h"
#include "pattern.h"
#include "message.h"

using CommandUMap = std::unordered_map<uint8_t, std::function<void(Message&)>>;


class Messenger {

    public:

        Messenger(queue_t &display_queue, queue_t &error_request_queue);
        void initialize();
        void update();

    protected:

        Pattern pat_;
        queue_t &display_queue_;
        queue_t &error_request_queue_;
        uint64_t msg_count_ = 0;
        uint64_t queue_drops_ = 0;       // S1.6: count display-pattern queue overflows
        uint64_t error_displayed_count_ = 0;  // # of error glyphs actually pushed to core 1
        uint64_t error_suppressed_count_ = 0; // # of error raises silently rate-limited
        uint64_t last_error_raised_us_ = 0;   // wall-clock of last raise (whether displayed or suppressed)
        bool     comm_check_ok_ = true;  // S1.4: COMM_CHECK byte-validation result; reset each update()

        CommandUMap cmd_umap_;
        void on_cmd_comms_check(Message &msg);
        void on_cmd_display_gray_2(Message &msg);
        void on_cmd_display_gray_16(Message &msg);
        void on_cmd_error_display(Message &msg);

        // Rate-limited error raise. Tries to enqueue a slot index for core 1
        // to display; under sustained errors (parity storm), additional
        // raises within ERROR_RATE_LIMIT_US are counted but not enqueued.
        // Returns true if the request was sent to core 1.
        bool raise_error(uint32_t slot);

};

#endif
