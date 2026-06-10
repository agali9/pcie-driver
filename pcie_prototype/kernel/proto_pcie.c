// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include "proto_pcie.h"
#include <linux/delay.h>

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

struct proto_pcie_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
    resource_size_t bar0_len;
    int irq;

    struct miscdevice miscdev;
    struct mutex lock;

    void *dma_buf;
    dma_addr_t dma_handle;
    size_t dma_len;
    u32 last_status;
};

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
            .ring_entries = 64,
            .ring_entry_size = 64,
        };
        if (copy_to_user((void __user *)arg, &info, sizeof(info)))
            ret = -EFAULT;
        break;
    }
    case PROTO_PCIE_IOC_RESET:
        proto_writel(d, REG_CTRL, CTRL_RESET);
        msleep(20);
        proto_writel(d, REG_CTRL, 0);
        d->last_status = proto_readl(d, REG_STATUS);
        break;
    case PROTO_PCIE_IOC_SEND_CMD: {
        struct proto_pcie_command cmdbuf;
        u32 n;

        if (copy_from_user(&cmdbuf, (void __user *)arg, sizeof(cmdbuf))) {
            ret = -EFAULT;
            break;
        }

        n = min_t(u32, cmdbuf.length, PROTO_PCIE_MAX_PAYLOAD);
        if (n > 0)
            memcpy(d->dma_buf, cmdbuf.payload, n);

        d->dma_len = n;
        proto_writel(d, REG_DMA_LO, lower_32_bits(d->dma_handle));
        proto_writel(d, REG_DMA_HI, upper_32_bits(d->dma_handle));
        proto_writel(d, REG_DMA_LEN, n);
        proto_writel(d, REG_DOORBELL, cmdbuf.opcode);
        proto_writel(d, REG_CTRL, CTRL_START);

        /* In a real device, completion would come from the interrupt. */
        d->last_status = proto_readl(d, REG_STATUS);
        break;
    }
    case PROTO_PCIE_IOC_GET_STATUS: {
        u32 status = d->last_status;
        if (copy_to_user((void __user *)arg, &status, sizeof(status)))
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
