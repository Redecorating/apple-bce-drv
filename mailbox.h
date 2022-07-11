#ifndef BCE_MAILBOX_H
#define BCE_MAILBOX_H

#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include <linux/completion.h>
#include <linux/pci.h>
#include <linux/timer.h>

struct apple_bridge_mbox {
    struct apple_bce_device *bce;
    struct mbox_controller controller;
    struct mbox_client client;
    struct mbox_chan chan;
    void __iomem *reg_mb;
    dma_addr_t result_dest;
};

/*
enum apple_bridge_mbox_status {
    APPLE_BRIDGE_MBOX_NO_MSG = 0,
    APPLE_BRIDGE_MBOX_ACTIVE_MSG = 1,
    APPLE_BRIDGE_MBOX_GOT_REPLY = 2,
};*/

struct apple_bridge_mbox_msg {
    int type; /* bce_message_type */
    u64 value;
};

enum bce_message_type {
    BCE_MB_REGISTER_COMMAND_SQ = 0x7,            // to-device
    BCE_MB_REGISTER_COMMAND_CQ = 0x8,            // to-device
    BCE_MB_REGISTER_COMMAND_QUEUE_REPLY = 0xB,   // to-host
    BCE_MB_SET_FW_PROTOCOL_VERSION = 0xC,        // both
    BCE_MB_SLEEP_NO_STATE = 0x14,                // to-device
    BCE_MB_RESTORE_NO_STATE = 0x15,              // to-device
    BCE_MB_SAVE_STATE_AND_SLEEP = 0x17,          // to-device
    BCE_MB_RESTORE_STATE_AND_WAKE = 0x18,        // to-device
    BCE_MB_SAVE_STATE_AND_SLEEP_FAILURE = 0x19,  // from-device
    BCE_MB_SAVE_RESTORE_STATE_COMPLETE = 0x1A,   // from-device
};

#define BCE_MB_MSG(type, value) (((u64) (type) << 58) | ((value) & 0x3FFFFFFFFFFFFFFLL))
#define BCE_MB_TYPE(v) ((u32) (v >> 58))
#define BCE_MB_VALUE(v) (v & 0x3FFFFFFFFFFFFFFLL)

int bce_mailbox_init(struct apple_bce_device *bce, void __iomem *reg_mb);
void bce_mailbox_exit(struct apple_bridge_mbox *mb);

struct bce_timestamp {
    void __iomem *reg;
    struct timer_list timer;
    struct spinlock stop_sl;
    bool stopped;
};

void bce_timestamp_init(struct bce_timestamp *ts, void __iomem *reg);

void bce_timestamp_start(struct bce_timestamp *ts, bool is_initial);

void bce_timestamp_stop(struct bce_timestamp *ts);

#endif //BCEDRIVER_MAILBOX_H
