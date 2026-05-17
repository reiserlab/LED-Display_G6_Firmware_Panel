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

        Messenger(queue_t &display_queue);
        void initialize();
        void update();

    protected:

        Pattern pat_;
        queue_t &display_queue_;
        uint64_t msg_count_ = 0;
        uint64_t queue_drops_ = 0;       // S1.6: count display-pattern queue overflows
        bool     comm_check_ok_ = true;  // S1.4: COMM_CHECK byte-validation result; reset each update()

        CommandUMap cmd_umap_;
        void on_cmd_comms_check(Message &msg);
        void on_cmd_display_gray_2(Message &msg);
        void on_cmd_display_gray_16(Message &msg);

};

#endif
