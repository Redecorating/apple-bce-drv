#include "apple_bce.h"
#include <linux/module.h>
#include <linux/crc32.h>

static dev_t bce_chrdev;
static struct class *bce_class;

struct apple_bce_device *global_bce;
EXPORT_SYMBOL_GPL(global_bce);

static int bce_create_command_queues(struct apple_bce_device *bce);
static void bce_free_command_queues(struct apple_bce_device *bce);
static irqreturn_t bce_handle_dma_irq(int irq, void *dev);
static int bce_fw_version_handshake(struct apple_bce_device *bce);
static int bce_register_command_queue(struct apple_bce_device *bce, struct bce_queue_memcfg *cfg, int is_sq);

static int apple_bce_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct apple_bce_device *bce = NULL;
    int status = 0;
    int nvec;

    pr_info("apple-bce: capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "apple-bce")) {
        status = -ENODEV;
        goto fail;
    }
    pci_set_master(dev);
    nvec = pci_alloc_irq_vectors(dev, 1, 8, PCI_IRQ_MSI);
    if (nvec < 5) {
        status = -EINVAL;
        goto fail;
    }

    bce = kzalloc(sizeof(struct apple_bce_device), GFP_KERNEL);
    if (!bce) {
        status = -ENOMEM;
        goto fail;
    }

    bce->pci = dev;
    pci_set_drvdata(dev, bce);

    bce->devt = bce_chrdev;
    bce->dev = device_create(bce_class, &dev->dev, bce->devt, NULL, "apple-bce");
    if (IS_ERR_OR_NULL(bce->dev)) {
        status = PTR_ERR(bce_class);
        goto fail;
    }

    bce->reg_mem_mb = pci_iomap(dev, 4, 0);
    bce->reg_mem_dma = pci_iomap(dev, 2, 0);

    if (IS_ERR_OR_NULL(bce->reg_mem_mb) || IS_ERR_OR_NULL(bce->reg_mem_dma)) {
        dev_warn(&dev->dev, "apple-bce: Failed to pci_iomap required regions\n");
        goto fail;
    }

    bce_timestamp_init(&bce->timestamp, bce->reg_mem_mb);

    spin_lock_init(&bce->queues_lock);
    ida_init(&bce->queue_ida);


    pr_err("asda\n");
    if ((status = bce_mailbox_init(bce, bce->reg_mem_mb)))
        goto fail;

    pr_err("asdpa\n");

    if ((status = pci_request_irq(dev, 4, NULL, bce_handle_dma_irq, dev, "bce_dma")))
        goto fail_interrupt_0;

    if ((status = dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(37)))) {
        dev_warn(&dev->dev, "dma: Setting mask failed\n");
        goto fail_interrupt;
    }

    /* Gets the function 0's interface. This is needed because Apple only accepts DMA on our function if function 0
       is a bus master, so we need to work around this. */
    bce->pci0 = pci_get_slot(dev->bus, PCI_DEVFN(PCI_SLOT(dev->devfn), 0));
#ifndef WITHOUT_NVME_PATCH
    if ((status = pci_enable_device_mem(bce->pci0))) {
        dev_warn(&dev->dev, "apple-bce: failed to enable function 0\n");
        goto fail_dev0;
    }
#endif
    pci_set_master(bce->pci0);

    bce_timestamp_start(&bce->timestamp, true);

    if ((status = bce_fw_version_handshake(bce)))
        goto fail_ts;
    pr_info("apple-bce: handshake done\n");

    if ((status = bce_create_command_queues(bce))) {
        pr_info("apple-bce: Creating command queues failed\n");
        goto fail_ts;
    }

    global_bce = bce;

    return 0;

fail_ts:
    bce_timestamp_stop(&bce->timestamp);
#ifndef WITHOUT_NVME_PATCH
    pci_disable_device(bce->pci0);
fail_dev0:
#endif
    pci_dev_put(bce->pci0);
fail_interrupt:
    pr_err("asd56655a\n");
    pci_free_irq(dev, 4, dev);
fail_interrupt_0:
    pr_err("asd555a\n");
    bce_mailbox_exit(&bce->mbox);
fail:
    pr_err("asd777a\n");
    if (bce && bce->dev) {
        device_destroy(bce_class, bce->devt);

        if (!IS_ERR_OR_NULL(bce->reg_mem_mb))
            pci_iounmap(dev, bce->reg_mem_mb);
        if (!IS_ERR_OR_NULL(bce->reg_mem_dma))
            pci_iounmap(dev, bce->reg_mem_dma);

        kfree(bce);
    }

    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}

static int bce_create_command_queues(struct apple_bce_device *bce)
{
    int status;
    struct bce_queue_memcfg *cfg;

    bce->cmd_cq = bce_alloc_cq(bce, 0, 0x20);
    bce->cmd_cmdq = bce_alloc_cmdq(bce, 1, 0x20);
    if (bce->cmd_cq == NULL || bce->cmd_cmdq == NULL) {
        status = -ENOMEM;
        goto err;
    }
    bce->queues[0] = (struct bce_queue *) bce->cmd_cq;
    bce->queues[1] = (struct bce_queue *) bce->cmd_cmdq->sq;

    cfg = kzalloc(sizeof(struct bce_queue_memcfg), GFP_KERNEL);
    if (!cfg) {
        status = -ENOMEM;
        goto err;
    }
    bce_get_cq_memcfg(bce->cmd_cq, cfg);
    if ((status = bce_register_command_queue(bce, cfg, false)))
        goto err;
    bce_get_sq_memcfg(bce->cmd_cmdq->sq, bce->cmd_cq, cfg);
    if ((status = bce_register_command_queue(bce, cfg, true)))
        goto err;
    kfree(cfg);

    return 0;

err:
    if (bce->cmd_cq)
        bce_free_cq(bce, bce->cmd_cq);
    if (bce->cmd_cmdq)
        bce_free_cmdq(bce, bce->cmd_cmdq);
    return status;
}

static void bce_free_command_queues(struct apple_bce_device *bce)
{
    bce_free_cq(bce, bce->cmd_cq);
    bce_free_cmdq(bce, bce->cmd_cmdq);
    bce->cmd_cq = NULL;
    bce->queues[0] = NULL;
}

static irqreturn_t bce_handle_dma_irq(int irq, void *dev)
{
    int i;
    struct apple_bce_device *bce = pci_get_drvdata(dev);
    spin_lock(&bce->queues_lock);
    for (i = 0; i < BCE_MAX_QUEUE_COUNT; i++)
        if (bce->queues[i] && bce->queues[i]->type == BCE_QUEUE_CQ)
            bce_handle_cq_completions(bce, (struct bce_queue_cq *) bce->queues[i]);
    spin_unlock(&bce->queues_lock);
    return IRQ_HANDLED;
}

static int bce_fw_version_handshake(struct apple_bce_device *bce)
{
    struct apple_bridge_mbox_msg msg;
    int status;

    msg.type = BCE_MB_SET_FW_PROTOCOL_VERSION;
    msg.value = BC_PROTOCOL_VERSION;
    status = mbox_send_message(&bce->mbox.chan, (void *)&msg);
    if (status)
        return status;

    if (msg.type != BCE_MB_SET_FW_PROTOCOL_VERSION ||
        msg.value != BC_PROTOCOL_VERSION) {
        pr_err("apple-bce: FW version handshake failed %x:%llx\n", msg.type, msg.value);
        return -EINVAL;
    }
    return 0;
}

static int bce_register_command_queue(struct apple_bce_device *bce, struct bce_queue_memcfg *cfg, int is_sq)
{
    struct apple_bridge_mbox_msg msg;
    int status;
    // OS X uses an bidirectional direction, but that's not really needed
    dma_addr_t a = dma_map_single(&bce->pci->dev, cfg, sizeof(struct bce_queue_memcfg), DMA_TO_DEVICE);
    if (dma_mapping_error(&bce->pci->dev, a))
        return -ENOMEM;

    msg.value = a;
    msg.type = is_sq ? BCE_MB_REGISTER_COMMAND_SQ : BCE_MB_REGISTER_COMMAND_CQ;
    status = mbox_send_message(&bce->mbox.chan, (void *)&msg);
    if (status)
        return status;

    dma_unmap_single(&bce->pci->dev, a, sizeof(struct bce_queue_memcfg), DMA_TO_DEVICE);
    if (status)
        return status;
    if (msg.type != BCE_MB_REGISTER_COMMAND_QUEUE_REPLY)
        return -EINVAL;
    return 0;
}

static void apple_bce_remove(struct pci_dev *dev)
{
    struct apple_bce_device *bce = pci_get_drvdata(dev);
    bce->is_being_removed = true;

    bce_timestamp_stop(&bce->timestamp);
#ifndef WITHOUT_NVME_PATCH
    pci_disable_device(bce->pci0);
#endif
    pci_dev_put(bce->pci0);
    bce_mailbox_exit(&bce->mbox);
    pci_free_irq(dev, 4, dev);
    bce_free_command_queues(bce);
    pci_iounmap(dev, bce->reg_mem_mb);
    pci_iounmap(dev, bce->reg_mem_dma);
    device_destroy(bce_class, bce->devt);
    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);
    kfree(bce);
}

static int bce_save_state_and_sleep(struct apple_bce_device *bce)
{
    struct apple_bridge_mbox_msg msg;
    int attempt, status = 0;
    dma_addr_t dma_addr;
    void *dma_ptr = NULL;
    size_t size = max(PAGE_SIZE, 4096UL);

    for (attempt = 0; attempt < 5; ++attempt) {
        pr_debug("apple-bce: suspend: attempt %i, buffer size %li\n", attempt, size);
        dma_ptr = dma_alloc_coherent(&bce->pci->dev, size, &dma_addr, GFP_KERNEL);
        if (!dma_ptr) {
            pr_err("apple-bce: suspend failed (data alloc failed)\n");
            break;
        }
        BUG_ON((dma_addr % 4096) != 0);
        msg.type = BCE_MB_SAVE_STATE_AND_SLEEP;
        msg.value = (dma_addr & ~(4096LLU - 1)) | (size / 4096);
        status = mbox_send_message(&bce->mbox.chan, (void *)&msg);
        if (status) {
            pr_err("apple-bce: suspend failed (mailbox send)\n");
            break;
        }
        if (msg.type == BCE_MB_SAVE_RESTORE_STATE_COMPLETE) {
            bce->saved_data_dma_addr = dma_addr;
            bce->saved_data_dma_ptr = dma_ptr;
            bce->saved_data_dma_size = size;
            return 0;
        } else if (msg.type == BCE_MB_SAVE_STATE_AND_SLEEP_FAILURE) {
            dma_free_coherent(&bce->pci->dev, size, dma_ptr, dma_addr);
            /* The 0x10ff magic value was extracted from Apple's driver */
            size = (msg.value + 0x10ff) & ~(4096LLU - 1);
            pr_debug("apple-bce: suspend: device requested a larger buffer (%li)\n", size);
            continue;
        } else {
            pr_err("apple-bce: suspend failed (invalid device response)\n");
            status = -EINVAL;
            break;
        }
    }
    if (dma_ptr)
        dma_free_coherent(&bce->pci->dev, size, dma_ptr, dma_addr);
    if (!status) {
        msg.type = BCE_MB_SLEEP_NO_STATE;
        msg.value = 0;
        return mbox_send_message(&bce->mbox.chan, (void *)&msg);
    }
    return status;
}

static int bce_restore_state_and_wake(struct apple_bce_device *bce)
{
    struct apple_bridge_mbox_msg msg;
    int status;
    if (!bce->saved_data_dma_ptr) {
       msg.type = BCE_MB_RESTORE_NO_STATE;
       msg.value = 0;
       status = mbox_send_message(&bce->mbox.chan, (void *)&msg);
       if (status) {
            pr_err("apple-bce: resume with no state failed (mailbox send)\n");
            return status;
        }
        if (msg.type != BCE_MB_RESTORE_NO_STATE) {
            pr_err("apple-bce: resume with no state failed (invalid device response)\n");
            return -EINVAL;
        }
        return 0;
    }

    msg.type = BCE_MB_RESTORE_STATE_AND_WAKE;
    msg.value = (bce->saved_data_dma_addr & ~(4096LLU - 1)) | (bce->saved_data_dma_size / 4096);
    status = mbox_send_message(&bce->mbox.chan, (void *)&msg);
    if (status) {
        pr_err("apple-bce: resume with state failed (mailbox send)\n");
        goto finish_with_state;
    }
    if (msg.type != BCE_MB_SAVE_RESTORE_STATE_COMPLETE) {
        pr_err("apple-bce: resume with state failed (invalid device response)\n");
        status = -EINVAL;
        goto finish_with_state;
    }

finish_with_state:
    dma_free_coherent(&bce->pci->dev, bce->saved_data_dma_size, bce->saved_data_dma_ptr, bce->saved_data_dma_addr);
    bce->saved_data_dma_ptr = NULL;
    return status;
}

static int apple_bce_suspend(struct device *dev)
{
    struct apple_bce_device *bce = pci_get_drvdata(to_pci_dev(dev));
    int status;

    bce_timestamp_stop(&bce->timestamp);

    if ((status = bce_save_state_and_sleep(bce)))
        return status;

    return 0;
}

static int apple_bce_resume(struct device *dev)
{
    struct apple_bce_device *bce = pci_get_drvdata(to_pci_dev(dev));
    int status;

    pci_set_master(bce->pci);
    pci_set_master(bce->pci0);

    if ((status = bce_restore_state_and_wake(bce)))
        return status;

    bce_timestamp_start(&bce->timestamp, false);

    return 0;
}

static struct pci_device_id apple_bce_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1801) },
        { 0, },
};

MODULE_DEVICE_TABLE(pci, apple_bce_ids);

struct dev_pm_ops apple_bce_pci_driver_pm = {
        .suspend = apple_bce_suspend,
        .resume = apple_bce_resume
};
struct pci_driver apple_bce_pci_driver = {
        .name = "apple-bce",
        .id_table = apple_bce_ids,
        .probe = apple_bce_probe,
        .remove = apple_bce_remove,
        .driver = {
                .pm = &apple_bce_pci_driver_pm
        }
};


static int __init apple_bce_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&bce_chrdev, 0, 1, "apple-bce")))
        goto fail_chrdev;
    bce_class = class_create(THIS_MODULE, "apple-bce");
    if (IS_ERR(bce_class)) {
        result = PTR_ERR(bce_class);
        goto fail_class;
    }

    result = pci_register_driver(&apple_bce_pci_driver);
    if (result)
        goto fail_drv;

    return 0;

fail_drv:
    pci_unregister_driver(&apple_bce_pci_driver);
fail_class:
    class_destroy(bce_class);
fail_chrdev:
    unregister_chrdev_region(bce_chrdev, 1);
    if (!result)
        result = -EINVAL;
    return result;
}
static void __exit apple_bce_module_exit(void)
{
    pci_unregister_driver(&apple_bce_pci_driver);

    class_destroy(bce_class);
    unregister_chrdev_region(bce_chrdev, 1);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MrARM");
MODULE_DESCRIPTION("Apple BCE Driver");
MODULE_VERSION("0.02");
module_init(apple_bce_module_init);
module_exit(apple_bce_module_exit);
