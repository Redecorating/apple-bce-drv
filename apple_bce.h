#ifndef APPLE_BCE_H
#define APPLE_BCE_H

#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/auxiliary_bus.h>
#include "mailbox.h"
#include "queue.h"

#define BC_PROTOCOL_VERSION 0x20001
#define BCE_MAX_QUEUE_COUNT 0x100

#define BCE_QUEUE_USER_MIN 2
#define BCE_QUEUE_USER_MAX (BCE_MAX_QUEUE_COUNT - 1)

#define BCE_VHCI_AUX_DEVICE_NAME "vhci"
#define BCE_AUDIO_AUX_DEVICE_NAME "audio"

struct apple_bce_device {
    struct pci_dev *pci, *pci0;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_mb;
    void __iomem *reg_mem_dma;
    struct bce_mailbox mbox;
    struct bce_timestamp timestamp;
    struct bce_queue *queues[BCE_MAX_QUEUE_COUNT];
    struct spinlock queues_lock;
    struct ida queue_ida;
    struct bce_queue_cq *cmd_cq;
    struct bce_queue_cmdq *cmd_cmdq;
    struct bce_queue_sq *int_sq_list[BCE_MAX_QUEUE_COUNT];
    bool is_being_removed;

    dma_addr_t saved_data_dma_addr;
    void *saved_data_dma_ptr;
    size_t saved_data_dma_size;

    struct auxiliary_device vhci_aux_dev, audio_aux_dev;
};

#endif //APPLE_BCE_H
