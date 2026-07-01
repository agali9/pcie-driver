// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/string.h>
#include "proto_pcie.h"

#define DRV_NAME "proto_pcie"

/* Placeholder IDs; replace with your QEMU or real device IDs. */
#define VENDOR_ID_PROTO 0x1AF4
#define DEVICE_ID_PROTO 0x1100

#define REG_CTRL        0x00
#define REG_STATUS      0x04
#define REG_DOORBELL    0x08
#define REG_IRQ_ACK     0x0C
#define REG_DMA_LO      0x10
#define REG_DMA_HI      0x14
#define REG_DMA_LEN     0x18
#define REG_VERSION     0x1C

#define CTRL_RESET      BIT(0)
#define CTRL_START      BIT(1)
#define STATUS_READY    BIT(0)
#define STATUS_DONE     BIT(1)
#define STATUS_ERROR    BIT(2)

#define PROTO_SIM_PAYLOAD_PREVIEW 48

static bool simulate = true;
module_param(simulate, bool, 0444);
MODULE_PARM_DESC(simulate, "Run in simulation mode without PCI hardware");

struct proto_sim_ring_entry {
    __u32 opcode;
    __u32 length;
    __u32 seqno;
    __u32 checksum;
    __u8 payload_preview[PROTO_SIM_PAYLOAD_PREVIEW];
};

struct proto_sim_completion_entry {
    __u32 opcode;
    __u32 status;
    __u32 length;
    __u32 seqno;
    __u8 reserved[PROTO_SIM_PAYLOAD_PREVIEW];
};

struct proto_pcie_dev {
    struct pci_dev *pdev;
    bool simulated;
    bool misc_registered;
    bool device_enabled;
    bool regions_requested;
    bool bar_mapped;
    bool irq_vectors_allocated;
    bool irq_requested;

    void __iomem *bar0;
    resource_size_t bar0_len;
    int irq;

    struct miscdevice miscdev;
    struct mutex lock;

    void *dma_buf;
    dma_addr_t dma_handle;
    size_t dma_len;
    u32 last_status;

    u32 ring_entries;
    u32 ring_entry_size;
    struct proto_sim_ring_entry *cmd_ring;
    struct proto_sim_completion_entry *comp_ring;
    u8 *cmd_payloads;
    u32 cmd_head;
    u32 cmd_tail;
    u32 cmd_pending;
    u32 comp_head;
    u32 comp_tail;
    u32 comp_pending;
    u32 completed_total;
    u32 last_opcode;
    u32 last_length;
    u32 next_seqno;
};

static struct proto_pcie_dev *proto_sim_dev;

static struct pci_device_id proto_pcie_tbl[] = {
    { PCI_DEVICE(VENDOR_ID_PROTO, DEVICE_ID_PROTO) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, proto_pcie_tbl);

static inline u32 proto_readl(struct proto_pcie_dev *d, u32 off)
{
    return ioread32(d->bar0 + off);
}

static inline void proto_writel(struct proto_pcie_dev *d, u32 off, u32 val)
{
    iowrite32(val, d->bar0 + off);
}

static u32 proto_checksum(const u8 *buf, u32 len)
{
    u32 sum = 0;
    u32 i;

    for (i = 0; i < len; ++i)
        sum += buf[i];

    return sum;
}

static void proto_log_mode(struct proto_pcie_dev *d, const char *msg)
{
    if (d->simulated)
        pr_info(DRV_NAME ": [sim] %s\n", msg);
    else if (d->pdev)
        dev_info(&d->pdev->dev, "%s\n", msg);
    else
        pr_info(DRV_NAME ": %s\n", msg);
}

static void proto_sim_clear_rings(struct proto_pcie_dev *d)
{
    if (d->cmd_ring)
        memset(d->cmd_ring, 0, sizeof(*d->cmd_ring) * d->ring_entries);
    if (d->comp_ring)
        memset(d->comp_ring, 0, sizeof(*d->comp_ring) * d->ring_entries);
    if (d->cmd_payloads)
        memset(d->cmd_payloads, 0, (size_t)d->ring_entries * PROTO_PCIE_MAX_PAYLOAD);

    d->cmd_head = 0;
    d->cmd_tail = 0;
    d->cmd_pending = 0;
    d->comp_head = 0;
    d->comp_tail = 0;
    d->comp_pending = 0;
    d->completed_total = 0;
    d->last_opcode = 0;
    d->last_length = 0;
    d->next_seqno = 1;
}

static void proto_finish_reset(struct proto_pcie_dev *d)
{
    if (d->simulated) {
        memset(d->dma_buf, 0, d->dma_len);
        proto_sim_clear_rings(d);
        d->last_status = STATUS_READY;
        return;
    }

    proto_writel(d, REG_CTRL, CTRL_RESET);
    msleep(20);
    proto_writel(d, REG_CTRL, 0);
    d->last_status = proto_readl(d, REG_STATUS);
}

static void proto_sim_store_completion(struct proto_pcie_dev *d,
                                       __u32 opcode,
                                       __u32 status,
                                       __u32 length,
                                       __u32 seqno)
{
    struct proto_sim_completion_entry *comp = &d->comp_ring[d->comp_tail];

    comp->opcode = opcode;
    comp->status = status;
    comp->length = length;
    comp->seqno = seqno;
    memset(comp->reserved, 0, sizeof(comp->reserved));

    d->comp_tail = (d->comp_tail + 1) % d->ring_entries;
    if (d->comp_pending < d->ring_entries)
        d->comp_pending++;
}

static int proto_sim_enqueue_command(struct proto_pcie_dev *d, struct proto_pcie_command *cmdbuf)
{
    struct proto_sim_ring_entry *entry;
    u32 slot;
    u32 n;
    u32 seqno;
    u8 *payload_slot;

    if (d->cmd_pending >= d->ring_entries)
        return -ENOSPC; /* ring full */

    slot = d->cmd_tail;
    entry = &d->cmd_ring[slot];
    payload_slot = d->cmd_payloads + ((size_t)slot * PROTO_PCIE_MAX_PAYLOAD);
    n = min_t(u32, cmdbuf->length, PROTO_PCIE_MAX_PAYLOAD);
    seqno = d->next_seqno++;

    entry->opcode = cmdbuf->opcode;
    entry->length = n;
    entry->seqno = seqno;
    entry->checksum = proto_checksum(cmdbuf->payload, n);
    memset(entry->payload_preview, 0, sizeof(entry->payload_preview));
    memcpy(entry->payload_preview, cmdbuf->payload,
           min_t(u32, n, sizeof(entry->payload_preview)));

    memset(payload_slot, 0, PROTO_PCIE_MAX_PAYLOAD);
    memcpy(payload_slot, cmdbuf->payload, n);

    d->cmd_tail = (d->cmd_tail + 1) % d->ring_entries;
    d->cmd_pending++;
    d->last_status = STATUS_READY;

    return 0;
}

static int proto_sim_process_pending(struct proto_pcie_dev *d)
{
    while (d->cmd_pending > 0) {
        struct proto_sim_ring_entry *entry = &d->cmd_ring[d->cmd_head];
        u8 *payload_slot = d->cmd_payloads + ((size_t)d->cmd_head * PROTO_PCIE_MAX_PAYLOAD);
        u32 n = entry->length;
        u32 copied = min_t(u32, n, (u32)d->dma_len);

        memset(d->dma_buf, 0, d->dma_len);
        memcpy(d->dma_buf, payload_slot, copied);
        d->last_opcode = entry->opcode;
        d->last_length = n;
        d->last_status = STATUS_READY | STATUS_DONE;
        d->completed_total++;

        proto_sim_store_completion(d, entry->opcode, d->last_status, n, entry->seqno);

        d->cmd_head = (d->cmd_head + 1) % d->ring_entries;
        d->cmd_pending--;
    }

    return 0;
}

static void proto_sim_get_ring_state(struct proto_pcie_dev *d, struct proto_pcie_ring_state *state)
{
    memset(state, 0, sizeof(*state));
    state->cmd_head = d->cmd_head;
    state->cmd_tail = d->cmd_tail;
    state->cmd_pending = d->cmd_pending;
    state->comp_head = d->comp_head;
    state->comp_tail = d->comp_tail;
    state->comp_pending = d->comp_pending;
    state->completed_total = d->completed_total;
    state->last_opcode = d->last_opcode;
    state->last_length = d->last_length;
    state->last_status = d->last_status;
    state->next_seqno = d->next_seqno;
}

static irqreturn_t proto_irq_handler(int irq, void *data)
{
    struct proto_pcie_dev *d = data;
    u32 status = proto_readl(d, REG_STATUS);

    d->last_status = status;
    proto_writel(d, REG_IRQ_ACK, status);

    return IRQ_HANDLED;
}

static long proto_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct proto_pcie_dev *d = file->private_data;
    long ret = 0;

    if (_IOC_TYPE(cmd) != PROTO_PCIE_IOC_MAGIC)
        return -ENOTTY;

    mutex_lock(&d->lock);

    switch (cmd) {
    case PROTO_PCIE_IOC_GET_INFO: {
        struct proto_pcie_info info = {
            .bar0_len = (u32)d->bar0_len,
            .irq = (u32)d->irq,
            .ring_entries = d->ring_entries,
            .ring_entry_size = d->ring_entry_size,
        };
        if (copy_to_user((void __user *)arg, &info, sizeof(info)))
            ret = -EFAULT;
        break;
    }
    case PROTO_PCIE_IOC_RESET:
        proto_finish_reset(d);
        break;
    case PROTO_PCIE_IOC_SEND_CMD: {
        struct proto_pcie_command cmdbuf;

        if (copy_from_user(&cmdbuf, (void __user *)arg, sizeof(cmdbuf))) {
            ret = -EFAULT;
            break;
        }

        if (d->simulated) {
            ret = proto_sim_enqueue_command(d, &cmdbuf);
            break;
        }

        /* Hardware path: write the command into the device registers. */
        if (d->dma_buf) {
            u32 n = min_t(u32, cmdbuf.length, PROTO_PCIE_MAX_PAYLOAD);
            memcpy(d->dma_buf, cmdbuf.payload, n);
            d->dma_len = n;
            proto_writel(d, REG_DMA_LO, lower_32_bits(d->dma_handle));
            proto_writel(d, REG_DMA_HI, upper_32_bits(d->dma_handle));
            proto_writel(d, REG_DMA_LEN, n);
            proto_writel(d, REG_DOORBELL, cmdbuf.opcode);
            proto_writel(d, REG_CTRL, CTRL_START);
            d->last_status = proto_readl(d, REG_STATUS);
        }
        break;
    }
    case PROTO_PCIE_IOC_PROCESS_PENDING:
        if (d->simulated)
            ret = proto_sim_process_pending(d);
        break;
    case PROTO_PCIE_IOC_GET_STATUS: {
        u32 status = d->last_status;
        if (copy_to_user((void __user *)arg, &status, sizeof(status)))
            ret = -EFAULT;
        break;
    }
    case PROTO_PCIE_IOC_GET_RING_STATE: {
        struct proto_pcie_ring_state state;

        if (!d->simulated) {
            ret = -ENOTSUPP;
            break;
        }

        proto_sim_get_ring_state(d, &state);
        if (copy_to_user((void __user *)arg, &state, sizeof(state)))
            ret = -EFAULT;
        break;
    }
    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&d->lock);
    return ret;
}

static int proto_open(struct inode *inode, struct file *file)
{
    struct miscdevice *mdev = file->private_data;
    struct proto_pcie_dev *d = container_of(mdev, struct proto_pcie_dev, miscdev);
    file->private_data = d;
    return 0;
}

static const struct file_operations proto_fops = {
    .owner = THIS_MODULE,
    .open = proto_open,
    .unlocked_ioctl = proto_ioctl,
};

static int proto_register_misc(struct proto_pcie_dev *d)
{
    d->miscdev.minor = MISC_DYNAMIC_MINOR;
    d->miscdev.name = DRV_NAME;
    d->miscdev.fops = &proto_fops;
    d->miscdev.parent = d->pdev ? &d->pdev->dev : NULL;

    return misc_register(&d->miscdev);
}

static void proto_destroy_device(struct proto_pcie_dev *d)
{
    if (!d)
        return;

    if (d->misc_registered)
        misc_deregister(&d->miscdev);

    if (d->irq_requested && d->irq > 0)
        free_irq(d->irq, d);

    if (d->irq_vectors_allocated && d->pdev)
        pci_free_irq_vectors(d->pdev);

    if (d->dma_buf) {
        if (d->simulated)
            kfree(d->dma_buf);
        else if (d->pdev)
            dma_free_coherent(&d->pdev->dev, d->dma_len, d->dma_buf, d->dma_handle);
    }

    if (d->cmd_ring)
        kfree(d->cmd_ring);
    if (d->comp_ring)
        kfree(d->comp_ring);
    if (d->cmd_payloads)
        kfree(d->cmd_payloads);

    if (d->bar_mapped)
        iounmap(d->bar0);

    if (d->regions_requested && d->pdev)
        pci_release_regions(d->pdev);

    if (d->device_enabled && d->pdev)
        pci_disable_device(d->pdev);

    kfree(d);
}

static int proto_sim_probe(void)
{
    struct proto_pcie_dev *d;
    int err;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    d->simulated = true;
    d->ring_entries = PROTO_PCIE_SIM_RING_ENTRIES;
    d->ring_entry_size = PROTO_PCIE_SIM_RING_ENTRY_SIZE;
    d->bar0_len = 0x1000;
    d->irq = 0;
    d->dma_len = 4096;
    d->dma_buf = kzalloc(d->dma_len, GFP_KERNEL);
    d->cmd_ring = kcalloc(d->ring_entries, sizeof(*d->cmd_ring), GFP_KERNEL);
    d->comp_ring = kcalloc(d->ring_entries, sizeof(*d->comp_ring), GFP_KERNEL);
    d->cmd_payloads = kcalloc(d->ring_entries, PROTO_PCIE_MAX_PAYLOAD, GFP_KERNEL);
    d->last_status = STATUS_READY;
    mutex_init(&d->lock);

    if (!d->dma_buf || !d->cmd_ring || !d->comp_ring || !d->cmd_payloads) {
        err = -ENOMEM;
        goto err_free;
    }

    proto_sim_clear_rings(d);

    err = proto_register_misc(d);
    if (err)
        goto err_free;

    d->misc_registered = true;
    proto_sim_dev = d;
    proto_log_mode(d, "simulation device ready at /dev/" DRV_NAME);
    return 0;

err_free:
    kfree(d->dma_buf);
    kfree(d->cmd_ring);
    kfree(d->comp_ring);
    kfree(d->cmd_payloads);
    kfree(d);
    return err;
}

static void proto_sim_remove(void)
{
    proto_destroy_device(proto_sim_dev);
    proto_sim_dev = NULL;
}

static int proto_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct proto_pcie_dev *d;
    int err;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    d->pdev = pdev;
    d->ring_entries = PROTO_PCIE_SIM_RING_ENTRIES;
    d->ring_entry_size = PROTO_PCIE_SIM_RING_ENTRY_SIZE;
    mutex_init(&d->lock);
    pci_set_drvdata(pdev, d);

    err = pci_enable_device_mem(pdev);
    if (err)
        goto err_free;
    d->device_enabled = true;

    err = pci_request_regions(pdev, DRV_NAME);
    if (err)
        goto err_destroy;
    d->regions_requested = true;

    d->bar0_len = pci_resource_len(pdev, 0);
    d->bar0 = pci_ioremap_bar(pdev, 0);
    if (!d->bar0) {
        err = -ENOMEM;
        goto err_destroy;
    }
    d->bar_mapped = true;

    err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (err)
        err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (err)
        goto err_destroy;

    d->dma_len = 4096;
    d->dma_buf = dma_alloc_coherent(&pdev->dev, d->dma_len, &d->dma_handle, GFP_KERNEL);
    if (!d->dma_buf) {
        err = -ENOMEM;
        goto err_destroy;
    }

    err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
    if (err < 0)
        goto err_destroy;
    d->irq_vectors_allocated = true;

    d->irq = pci_irq_vector(pdev, 0);
    err = request_irq(d->irq, proto_irq_handler, 0, DRV_NAME, d);
    if (err)
        goto err_destroy;
    d->irq_requested = true;

    err = proto_register_misc(d);
    if (err)
        goto err_destroy;
    d->misc_registered = true;

    pci_set_master(pdev);
    dev_info(&pdev->dev, "proto_pcie ready: bar0=%pa len=%pa irq=%d\n",
             &pci_resource_start(pdev, 0), &d->bar0_len, d->irq);
    return 0;

err_destroy:
    proto_destroy_device(d);
    return err;

err_free:
    kfree(d);
    return err;
}

static void proto_pci_remove(struct pci_dev *pdev)
{
    struct proto_pcie_dev *d = pci_get_drvdata(pdev);

    if (!d)
        return;

    proto_destroy_device(d);
}

static struct pci_driver proto_pci_driver = {
    .name = DRV_NAME,
    .id_table = proto_pcie_tbl,
    .probe = proto_pci_probe,
    .remove = proto_pci_remove,
};

static int __init proto_init(void)
{
    if (simulate)
        return proto_sim_probe();

    return pci_register_driver(&proto_pci_driver);
}

static void __exit proto_exit(void)
{
    if (simulate)
        proto_sim_remove();
    else
        pci_unregister_driver(&proto_pci_driver);
}

module_init(proto_init);
module_exit(proto_exit);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Prototype PCIe driver with userspace ioctl interface");
MODULE_LICENSE("GPL");
