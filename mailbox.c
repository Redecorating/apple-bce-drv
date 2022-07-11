#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include "mailbox.h"
//#include <linux/atomic.h>
#include "apple_bce.h"

#define REG_MBOX_OUT_BASE 0x820
#define REG_MBOX_REPLY_COUNTER 0x108
#define REG_MBOX_REPLY_BASE 0x810
#define REG_TIMESTAMP_BASE 0xC000

#define BCE_MBOX_TIMEOUT_MS 200

/* Mailbox */

static int apple_bridge_mbox_retrive_response(struct apple_bridge_mbox *mb)
{
    u32 __iomem *regb;
    u64 result;
    u32 lo, hi;
    int count;
    u32 res = ioread32((u8*) mb->reg_mb + REG_MBOX_REPLY_COUNTER);
    count = (res >> 20) & 0xf;
    if (!count)
    	return -ENODATA;
    else {
        BUG_ON(count != 1); /* We don't send more than one message at once, this shouldn't happen. */
        regb = (u32*) ((u8*) mb->reg_mb + REG_MBOX_REPLY_BASE);
        lo = ioread32(regb);
        hi = ioread32(regb + 1);
        ioread32(regb + 2); //TODO: are these needed?
        ioread32(regb + 3);
        pr_debug("apple_bridge_mbox_retrive_response %llx\n", ((u64) hi << 32) | lo);
        result = ((u64) hi << 32) | lo;

        mbox_chan_received_data(&mb->chan, (void *)&result);
		mbox_chan_txdone(&mb->chan, 0); //TODO 0?
    	return 0;
    }

}

static irqreturn_t apple_bridge_mbox_handle_interrupt(int irq, void *dev)
{
    struct apple_bce_device *bce = pci_get_drvdata(dev);
    struct apple_bridge_mbox *mb = &bce->mbox;
    apple_bridge_mbox_retrive_response(mb);
    return IRQ_HANDLED;
}

int apple_bridge_mbox_startup(struct mbox_chan *chan) {
	struct apple_bridge_mbox *mb = (struct apple_bridge_mbox *)chan->con_priv;
	int ret = 0;

	if (!mb)
		return -EINVAL;

	pr_err("getting irq\n");
	ret = pci_request_irq(mb->bce->pci, 0, apple_bridge_mbox_handle_interrupt,
			NULL, mb->bce->pci, "apple_bridge_mbox");
	return ret;
}

void apple_bridge_mbox_shutdown(struct mbox_chan *chan) {
	struct apple_bridge_mbox *mb = (struct apple_bridge_mbox *)chan->con_priv;
	pr_err("releasing irq\n");
	pci_free_irq(mb->bce->pci, 0, mb->bce->pci);
}

int apple_bridge_mbox_send_data(struct mbox_chan *chan, void *data) {
	struct apple_bridge_mbox *mb = (struct apple_bridge_mbox *)chan->con_priv;
	struct apple_bridge_mbox_msg *msg = data;
    u64 __msg;
    u32 __iomem *regb;

	mb->result_dest = (dma_addr_t)data;

	__msg = ((u64) (msg->type) << 58) | ((msg->value) & 0x3FFFFFFFFFFFFFFLL);


    /*
    if (atomic_cmpxchg(&mb->mb_status, 0, 1) != 0) { //TODO use enum
        return -EBUSY; // We don't support two messages at once
    }*/

    regb = (u32*) ((u8*) mb->reg_mb + REG_MBOX_OUT_BASE);
    iowrite32((u32) __msg, regb);
    iowrite32((u32) (__msg >> 32), regb + 1);
    iowrite32(0, regb + 2);
    iowrite32(0, regb + 3);

	return 0;
}

void apple_bridge_mbox_rx_callback(struct mbox_client *cl, void *_msg) {
    struct apple_bridge_mbox *mb = container_of(cl, struct apple_bridge_mbox, client);
    struct apple_bridge_mbox_msg *result = (struct apple_bridge_mbox_msg *)mb->result_dest;
    u64 msg = *((u64 *)_msg);

    result->type = (u32) (msg >> 58);
    result->value = msg & 0x3FFFFFFFFFFFFFFLL;
}

static const struct mbox_chan_ops apple_bridge_mbox_ops = {
	.send_data = apple_bridge_mbox_send_data,
	.startup = apple_bridge_mbox_startup,
	.shutdown = apple_bridge_mbox_shutdown,
};

int bce_mailbox_init(struct apple_bce_device *bce, void __iomem *reg_mb)
{
    struct apple_bridge_mbox *mb = &bce->mbox;
    int ret;

    mb->bce = bce;
    mb->reg_mb = reg_mb;

	mb->controller.dev = mb->bce->dev;
	mb->controller.ops = &apple_bridge_mbox_ops;
	mb->controller.chans = &mb->chan;
	mb->controller.num_chans = 1;
	mb->controller.txdone_irq = true; //TODO check

    pr_err("reg controller\n");
    ret = devm_mbox_controller_register(mb->bce->dev, &mb->controller);
    if (ret)
    	return ret;
    pr_err("reg controller done\n");
    mbox_controller_unregister(&mb->controller);
    pr_err("dsfdsg\n\n");
    return -EINVAL;

    mb->chan.con_priv = mb;

    mb->client.dev = mb->bce->dev;
    mb->client.rx_callback = apple_bridge_mbox_rx_callback;
    mb->client.tx_block = true;
    mb->client.tx_tout = BCE_MBOX_TIMEOUT_MS;

    pr_err("a\n");
    mbox_request_channel(&mb->client, 0);
    pr_err("b\n");

    return 0;
}
void bce_mailbox_exit(struct apple_bridge_mbox *mb) {
    //mbox_free_channel(&mb->chan);
    mbox_controller_unregister(&mb->controller);
}
// BCE_MBOX_TIMEOUT_MS is client tx_tout

/* Timestamp */

static void bc_send_timestamp(struct timer_list *tl);

void bce_timestamp_init(struct bce_timestamp *ts, void __iomem *reg)
{
    u32 __iomem *regb;

    spin_lock_init(&ts->stop_sl);
    ts->stopped = false;

    ts->reg = reg;

    regb = (u32*) ((u8*) ts->reg + REG_TIMESTAMP_BASE);

    ioread32(regb);
    mb();

    timer_setup(&ts->timer, bc_send_timestamp, 0);
}

void bce_timestamp_start(struct bce_timestamp *ts, bool is_initial)
{
    unsigned long flags;
    u32 __iomem *regb = (u32*) ((u8*) ts->reg + REG_TIMESTAMP_BASE);

    if (is_initial) {
        iowrite32((u32) -4, regb + 2);
        iowrite32((u32) -1, regb);
    } else {
        iowrite32((u32) -3, regb + 2);
        iowrite32((u32) -1, regb);
    }

    spin_lock_irqsave(&ts->stop_sl, flags);
    ts->stopped = false;
    spin_unlock_irqrestore(&ts->stop_sl, flags);
    mod_timer(&ts->timer, jiffies + msecs_to_jiffies(150));
}

void bce_timestamp_stop(struct bce_timestamp *ts)
{
    unsigned long flags;
    u32 __iomem *regb = (u32*) ((u8*) ts->reg + REG_TIMESTAMP_BASE);

    spin_lock_irqsave(&ts->stop_sl, flags);
    ts->stopped = true;
    spin_unlock_irqrestore(&ts->stop_sl, flags);
    del_timer_sync(&ts->timer);

    iowrite32((u32) -2, regb + 2);
    iowrite32((u32) -1, regb);
}

static void bc_send_timestamp(struct timer_list *tl)
{
    struct bce_timestamp *ts;
    unsigned long flags;
    u32 __iomem *regb;
    ktime_t bt;

    ts = container_of(tl, struct bce_timestamp, timer);
    regb = (u32*) ((u8*) ts->reg + REG_TIMESTAMP_BASE);
    local_irq_save(flags);
    ioread32(regb + 2);
    mb();
    bt = ktime_get_boottime();
    iowrite32((u32) bt, regb + 2);
    iowrite32((u32) (bt >> 32), regb);

    spin_lock(&ts->stop_sl);
    if (!ts->stopped)
        mod_timer(&ts->timer, jiffies + msecs_to_jiffies(150));
    spin_unlock(&ts->stop_sl);
    local_irq_restore(flags);
}
