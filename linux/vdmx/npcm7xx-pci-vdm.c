// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2014-2018 Nuvoton Technology corporation.

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/kfifo.h>
#include <linux/poll.h>

#define DEVICE_NAME	"npcm7xx-vdm"

/* GCR  Register */
#define MFSEL3 0x64
#define  MFSEL3_PCIE_PUSE BIT(17)
#define INTCR3 0x9C
#define  INTCR3_PCIRREL BIT(30)

/* VDM  Register */
#define VDMX_BA 0xE0800000
#define VDMX_STATR 0x0000
#define  VDMX_STATR_RXNDW	GENMASK(23, 16)
#define  VDMX_STATR_RXNDW_OFFSET	16
#define  VDMX_STATR_RXDR BIT(2)
#define  VDMX_STATR_RXF BIT(1)
#define  VDMX_STATR_TXS BIT(0)
#define VDMX_IEN 0x0004
#define  VDMX_IEN_RXDREN BIT(2)
#define  VDMX_IEN_RXFEN BIT(1)
#define  VDMX_IEN_TXSEN BIT(0)
#define VDMX_RXF 0x0008
#define VDMX_TXF 0x000C
#define VDMX_CNT 0x0010
#define  VDMX_CNT_RXTO_05US BIT(4)
#define  VDMX_CNT_RXTO_1US  BIT(5)
#define  VDMX_CNT_RXTO_2US  (BIT(5) | BIT(4))
#define  VDMX_CNT_RXTO_4US  BIT(6)
#define  VDMX_CNT_RXTO_8US  (BIT(6) | BIT(4))
#define  VDMX_CNT_VDMXEN    BIT(1)
#define  VDMX_CNT_TXP   BIT(0)
#define VDMX_RXFILT	0x0014
#define  VDMX_RXFILT_FEN    BIT(31)
#define  VDMX_VENDOR_ID     0xb4a1

#define VDMX_RX_LEN	4
#define VDMX_TX_LEN	4
#define VDMX_KFIFO_SIZE	(16 * 1024)

typedef void *(*copy_func_t)(void *dest, const void *src, size_t n);

struct npcm7xx_vdm {
	struct device *dev;
	struct miscdevice miscdev;
	spinlock_t lock;
	int is_open;
	int irq;
	void __iomem *vdm_base;
	u32 *virt;
	struct regmap *gcr_regmap;
	wait_queue_head_t	queue;
	u8 *txbuf;
	struct kfifo	fifo;
};

static atomic_t npcm7xx_vdm_open_count = ATOMIC_INIT(0);

static struct npcm7xx_vdm *file_vdm(struct file *file)
{
	return container_of(file->private_data, struct npcm7xx_vdm, miscdev);
}

static void vdm_update(struct npcm7xx_vdm *vdm, u32 reg,
		       unsigned long mask, u32 bits)
{
	u32 t = ioread32(vdm->vdm_base + reg);

	t &= ~mask;
	t |= (bits & mask);
	iowrite32(t, vdm->vdm_base + reg);
}

static u32 vdm_read(struct npcm7xx_vdm *vdm, u32 reg)
{
	u32 t = ioread32(vdm->vdm_base + reg);

	return t;
}

static void vdm_write(struct npcm7xx_vdm *vdm, u32 reg, u32 val)
{
	iowrite32(val, vdm->vdm_base + reg);
}

static irqreturn_t npcm7xx_vdm_irq(int irq, void *arg)
{
	struct npcm7xx_vdm *vdm = arg;
	u32 status = 0;
	u32 val = 0;

	status = vdm_read(vdm, VDMX_STATR);

	if ((status & VDMX_STATR_RXDR) || (status & VDMX_STATR_RXF)) {
		dev_dbg(vdm->dev, "VDMX_STATR_RXF\n");
		while ((val = vdm_read(vdm, VDMX_RXF)) != 0xffffffff ||
			((status & VDMX_STATR_RXNDW) >> VDMX_STATR_RXNDW_OFFSET)) {
			while (kfifo_avail(&vdm->fifo) < VDMX_RX_LEN)
				kfifo_skip(&vdm->fifo);
			kfifo_in(&vdm->fifo, &val, VDMX_RX_LEN);
		}
	}

	if (status & VDMX_STATR_TXS)
		dev_dbg(vdm->dev, "VDMX_STATR_TXS\n");

	vdm_write(vdm, VDMX_STATR, status);

	wake_up(&vdm->queue);

	return IRQ_HANDLED;
}

static int npcm7xx_vdm_init(struct npcm7xx_vdm *vdm)
{
	int ret = 0;
	u32 reg_val = 0;
	struct regmap *gcr = vdm->gcr_regmap;

	/* Configure PCIE phy selection for bridge */
	regmap_update_bits(gcr, MFSEL3, MFSEL3_PCIE_PUSE, ~MFSEL3_PCIE_PUSE);

	/* PCIE reset if forced off */
	regmap_update_bits(gcr, INTCR3, INTCR3_PCIRREL, INTCR3_PCIRREL);

	/* Clear VDM STAT */
	vdm_write(vdm, VDMX_STATR,
		  VDMX_STATR_RXDR | VDMX_STATR_RXF | VDMX_STATR_TXS);

	/* VDM RESET */
	vdm_write(vdm, VDMX_CNT, 0);
	reg_val = VDMX_CNT_RXTO_8US | VDMX_CNT_VDMXEN;
	vdm_write(vdm, VDMX_CNT, reg_val);

	/* VDM Filter */
	reg_val = 0;//VDMX_RXFILT_FEN | VDMX_VENDOR_ID;
	vdm_write(vdm, VDMX_RXFILT, reg_val);

	vdm_write(vdm, VDMX_IEN, VDMX_IEN_RXFEN | VDMX_IEN_RXDREN);

	return ret;
}

static ssize_t npcm7xx_vdm_read(struct file *file, char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct npcm7xx_vdm *vdm = file_vdm(file);
	unsigned int copied;
	int ret = 0;
	int cond_size = 1;

	if (kfifo_len(&vdm->fifo) < cond_size) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible
			(vdm->queue, kfifo_len(&vdm->fifo) >= cond_size);
		if (ret == -ERESTARTSYS)
			return -EINTR;
	}

	ret = kfifo_to_user(&vdm->fifo, buffer, count, &copied);

	return ret ? ret : copied;
}

static int
npcm7xx_vdm_send(struct npcm7xx_vdm *vdm, u8 *txbuf, int size)
{
	u32 data;
	int i, ret;

	for (i = 0; i < size; i += VDMX_TX_LEN) {
		memcpy(&data, txbuf + i, VDMX_TX_LEN);
		vdm_write(vdm, VDMX_TXF, data);
	}

	vdm_update(vdm, VDMX_CNT, VDMX_CNT_TXP, VDMX_CNT_TXP);

	ret = wait_event_interruptible(vdm->queue,
				       !(vdm_read(vdm, VDMX_CNT) &
						VDMX_CNT_TXP));
	if (ret == -ERESTARTSYS)
		return -EINTR;

	vdm_update(vdm, VDMX_STATR, VDMX_STATR_TXS, VDMX_STATR_TXS);

	return size;
}

static ssize_t npcm7xx_vdm_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct npcm7xx_vdm *vdm = file_vdm(file);
	unsigned long flags;
	unsigned int buffsize;

	if (!access_ok(buf, count))
		return -EFAULT;

	spin_lock_irqsave(&vdm->lock, flags);

	vdm->txbuf = kzalloc(count + VDMX_TX_LEN, GFP_KERNEL);
	if (!vdm->txbuf) {
		spin_unlock_irqrestore(&vdm->lock, flags);
		return -ENOMEM;
	}

	if (copy_from_user((void *)(vdm->txbuf + *ppos),
			   (void __user *)buf, count)) {
		dev_err(vdm->dev, "copy_from_user error\n");
		spin_unlock_irqrestore(&vdm->lock, flags);
		kfree(vdm->txbuf);
		return -EFAULT;
	}

	npcm7xx_vdm_send(vdm, vdm->txbuf, count);

	kfree(vdm->txbuf);
	spin_unlock_irqrestore(&vdm->lock, flags);
	return count;
}

static int npcm7xx_vdm_open(struct inode *inode, struct file *file)
{
	if (atomic_inc_return(&npcm7xx_vdm_open_count) == 1)
		return 0;

	atomic_dec(&npcm7xx_vdm_open_count);
	return -EBUSY;
}

static int npcm7xx_vdm_release(struct inode *inode, struct file *file)
{
	atomic_dec(&npcm7xx_vdm_open_count);
	return 0;
}

static unsigned int npcm7xx_vdm_poll(struct file *file, poll_table *wait)
{
	struct npcm7xx_vdm *vdm = file_vdm(file);
	unsigned int mask = 0;

	poll_wait(file, &vdm->queue, wait);
	if (!kfifo_is_empty(&vdm->fifo))
		mask |= POLLIN;

	return mask;
}

const struct file_operations npcm87xx_vdm_fops = {
	.llseek = noop_llseek,
	.open = npcm7xx_vdm_open,
	.read = npcm7xx_vdm_read,
	.write = npcm7xx_vdm_write,
	.release = npcm7xx_vdm_release,
	.poll = npcm7xx_vdm_poll,
};

static int npcm7xx_vdm_config_irq(struct npcm7xx_vdm *vdm,
				  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc, irq;

	irq = irq_of_parse_and_map(dev->of_node, 0);
	if (!irq)
		return -ENODEV;

	rc = devm_request_irq(dev, irq, npcm7xx_vdm_irq, 0, DEVICE_NAME, vdm);
	if (rc < 0) {
		dev_err(dev, "Unable to request IRQ %d\n", irq);
		return rc;
	}

	return 0;
}

static int npcm_vdm_probe(struct platform_device *pdev)
{
	struct npcm7xx_vdm *vdm;
	struct resource *res;
	struct device *dev;
	int ret;

	dev = &pdev->dev;

	vdm = devm_kzalloc(dev, sizeof(*vdm), GFP_KERNEL);
	if (!vdm)
		return -ENOMEM;

	vdm->dev = dev;
	dev_set_drvdata(&pdev->dev, vdm);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "VDM reg resource not found\n");
		ret = -ENODEV;
		goto err;
	}

	vdm->vdm_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(vdm->vdm_base))
		return PTR_ERR(vdm->vdm_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(dev, "VDMA reg resource not found\n");
		ret = -ENODEV;
		goto err;
	}

	vdm->gcr_regmap =
		syscon_regmap_lookup_by_compatible("nuvoton,npcm750-gcr");
	if (IS_ERR(vdm->gcr_regmap)) {
		dev_err(dev, "failed to find nuvoton,npcm750-gcr\n");
		return PTR_ERR(vdm->gcr_regmap);
	}

	spin_lock_init(&vdm->lock);
	init_waitqueue_head(&vdm->queue);

	ret = kfifo_alloc(&vdm->fifo,
			  VDMX_KFIFO_SIZE, GFP_KERNEL);
	if (ret) {
		dev_err(dev, "failed to alloc kfifo\n");
		return ret;
	}

	/* register miscdev */
	vdm->miscdev.parent = dev;
	vdm->miscdev.fops =  &npcm87xx_vdm_fops;
	vdm->miscdev.minor = MISC_DYNAMIC_MINOR;
	vdm->miscdev.name = DEVICE_NAME;
	ret = misc_register(&vdm->miscdev);
	if (ret) {
		dev_err(dev,
			"Unable to register device, ret %d\n", ret);
		goto err;
	}

	ret = npcm7xx_vdm_config_irq(vdm, pdev);
	if (ret) {
		dev_err(dev, "Failed to configure IRQ\n");
		misc_deregister(&vdm->miscdev);
		goto err;
	}

	ret = npcm7xx_vdm_init(vdm);
	if (ret) {
		dev_err(dev, "Failed to init VDM\n");
		misc_deregister(&vdm->miscdev);
		goto err;
	}

	pr_info("NPCM7XX VDM Driver probed\n");

	return 0;
err:
	kfree(vdm);
	return ret;
}

static int npcm_vdm_remove(struct platform_device *pdev)
{
	struct npcm7xx_vdm *vdm = platform_get_drvdata(pdev);

	if (!vdm)
		return 0;

	misc_deregister(&vdm->miscdev);
	kfree(vdm->miscdev.name);
	kfifo_free(&vdm->fifo);
	kfree(vdm);

	return 0;
}

static const struct of_device_id npcm7xx_vdm_match[] = {
	{ .compatible = "nuvoton,npcm750-vdm", },
	{},
};

static struct platform_driver npcm_vdm_driver = {
	.probe          = npcm_vdm_probe,
	.remove			= npcm_vdm_remove,
	.driver         = {
		.name   = DEVICE_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = npcm7xx_vdm_match,
	},
};

module_platform_driver(npcm_vdm_driver);

MODULE_DEVICE_TABLE(of, npcm7xx_vdm_match);
MODULE_AUTHOR("Nuvoton Technology Corp.");
MODULE_DESCRIPTION("VDM Master Driver");
MODULE_LICENSE("GPL");
