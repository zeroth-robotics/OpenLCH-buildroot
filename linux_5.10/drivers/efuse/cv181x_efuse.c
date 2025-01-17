// SPDX-License-Identifier: GPL-2.0+
//
// EFUSE implementation
//

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>

#include "linux/cv180x_efuse.h"

#define ERROR(fmt, ...) pr_err(fmt, ##__VA_ARGS__)
#define VERBOSE(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)

#define EFUSE_BASE (0x03050000)

#define EFUSE_SHADOW_REG (efuse_base + 0x100)
#define EFUSE_SIZE 0x100

#define EFUSE_MODE (efuse_base + 0x0)
#define EFUSE_ADR (efuse_base + 0x4)
#define EFUSE_DIR_CMD (efuse_base + 0x8)
#define EFUSE_RD_DATA (efuse_base + 0xC)
#define EFUSE_STATUS (efuse_base + 0x10)
#define EFUSE_ONE_WAY (efuse_base + 0x14)

#define EFUSE_BIT_AREAD BIT(0)
#define EFUSE_BIT_MREAD BIT(1)
#define EFUSE_BIT_PRG BIT(2)
#define EFUSE_BIT_PWR_DN BIT(3)
#define EFUSE_BIT_CMD BIT(4)
#define EFUSE_BIT_BUSY BIT(0)
#define EFUSE_CMD_REFRESH (0x30)

static dev_t efuse_dev;
static struct cdev efuse_cdev;
static struct class *efuse_class;

#define EFUSE_IOC_MAGIC 'E'
#define EFUSE_IOC_READ _IOR(EFUSE_IOC_MAGIC, 1, struct efuse_data)
#define EFUSE_IOC_WRITE _IOW(EFUSE_IOC_MAGIC, 2, struct efuse_data)

struct efuse_data {
    uint32_t addr;
    uint32_t value;
};

enum EFUSE_READ_TYPE { EFUSE_AREAD, EFUSE_MREAD };

static void __iomem *efuse_base;
static struct clk *efuse_clk;

static inline void mmio_write_32(void __iomem *addr, uint32_t value)
{
	iowrite32(value, addr);
}

static inline uint32_t mmio_read_32(void __iomem *addr)
{
	return ioread32(addr);
}

static inline void mmio_setbits_32(void __iomem *addr, uint32_t set)
{
	mmio_write_32(addr, mmio_read_32(addr) | set);
}

void cvi_efuse_wait_for_ready(void)
{
	while (mmio_read_32(EFUSE_STATUS) & EFUSE_BIT_BUSY)
		;
}

static void cvi_efuse_power_on(uint32_t on)
{
	if (on)
		mmio_setbits_32(EFUSE_MODE, EFUSE_BIT_CMD);
	else
		mmio_setbits_32(EFUSE_MODE, EFUSE_BIT_PWR_DN | EFUSE_BIT_CMD);
}

static void cvi_efuse_refresh(void)
{
	mmio_write_32(EFUSE_MODE, EFUSE_CMD_REFRESH);
}

static void cvi_efuse_prog_bit(uint32_t word_addr, uint32_t bit_addr,
			       uint32_t high_row)
{
	uint32_t phy_addr;

	// word_addr: virtual addr, take "lower 6-bits" from 7-bits (0-127)
	// bit_addr: virtual addr, 5-bits (0-31)

	// composite physical addr[11:0] = [11:7]bit_addr + [6:0]word_addr
	phy_addr =
		((bit_addr & 0x1F) << 7) | ((word_addr & 0x3F) << 1) | high_row;

	cvi_efuse_wait_for_ready();

	// send efuse program cmd
	mmio_write_32(EFUSE_ADR, phy_addr);
	mmio_write_32(EFUSE_MODE, EFUSE_BIT_PRG | EFUSE_BIT_CMD);
}

static uint32_t cvi_efuse_read_from_phy(uint32_t phy_word_addr,
					enum EFUSE_READ_TYPE type)
{
	// power on efuse macro
	cvi_efuse_power_on(1);

	cvi_efuse_wait_for_ready();

	mmio_write_32(EFUSE_ADR, phy_word_addr);

	if (type == EFUSE_AREAD) // array read
		mmio_write_32(EFUSE_MODE, EFUSE_BIT_AREAD | EFUSE_BIT_CMD);
	else if (type == EFUSE_MREAD) // margin read
		mmio_write_32(EFUSE_MODE, EFUSE_BIT_MREAD | EFUSE_BIT_CMD);
	else {
		ERROR("EFUSE: Unsupported read type!");
		return (uint32_t)-1;
	}

	cvi_efuse_wait_for_ready();

	return mmio_read_32(EFUSE_RD_DATA);
}

static int cvi_efuse_write_word(uint32_t vir_word_addr, uint32_t val)
{
	uint32_t i, j, row_val, zero_bit;
	uint32_t new_value;
	int err_cnt = 0;

	for (j = 0; j < 2; j++) {
		VERBOSE("EFUSE: Program physical word addr #%d\n",
			(vir_word_addr << 1) | j);

		// array read by word address
		row_val = cvi_efuse_read_from_phy(
			(vir_word_addr << 1) | j,
			EFUSE_AREAD); // read low word of word_addr
		zero_bit = val & (~row_val); // only program zero bit

		// program row which bit is zero
		for (i = 0; i < 32; i++) {
			if ((zero_bit >> i) & 1)
				cvi_efuse_prog_bit(vir_word_addr, i, j);
		}

		// check by margin read
		new_value = cvi_efuse_read_from_phy((vir_word_addr << 1) | j,
						    EFUSE_MREAD);
		VERBOSE("%s(): val=0x%x new_value=0x%x\n", __func__, val,
			new_value);
		if ((val & new_value) != val) {
			err_cnt += 1;
			ERROR("EFUSE: Program bits check failed (%d)!\n",
			      err_cnt);
		}
	}

	cvi_efuse_refresh();

	return err_cnt >= 2 ? -EIO : 0;
}


static int efuse_open(struct inode *inode, struct file *file)
{
	// No special initialization needed
	return 0;
}

static int efuse_release(struct inode *inode, struct file *file)
{
	// No special cleanup needed
	return 0;
}

static ssize_t efuse_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	int ret;
	char kernel_buf[EFUSE_SIZE];

	if (*offset >= EFUSE_SIZE)
		return 0;  // EOF

	if (*offset + count > EFUSE_SIZE)
		count = EFUSE_SIZE - *offset;

	ret = cvi_efuse_read_buf(*offset, kernel_buf, count);
	if (ret < 0)
		return ret;

	if (copy_to_user(buf, kernel_buf, ret))
		return -EFAULT;

	*offset += ret;
	return ret;
}

static ssize_t efuse_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	int ret;
	uint32_t addr, value;

	if (count != sizeof(uint32_t) * 2)
		return -EINVAL;

	if (copy_from_user(&addr, buf, sizeof(uint32_t)))
		return -EFAULT;

	if (copy_from_user(&value, buf + sizeof(uint32_t), sizeof(uint32_t)))
		return -EFAULT;

	ret = cvi_efuse_write(addr, value);
	if (ret < 0)
		return ret;

	*offset += count;
	return count;
}

static long efuse_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct efuse_data data;
	int64_t ret;

	if (copy_from_user(&data, (struct efuse_data __user *)arg, sizeof(data)))
		return -EFAULT;

	switch (cmd) {
	case EFUSE_IOC_READ:
		ret = cvi_efuse_read_from_shadow(data.addr);
		if (ret < 0)
			return ret;
		data.value = ret;
		if (copy_to_user((struct efuse_data __user *)arg, &data, sizeof(data)))
			return -EFAULT;
		break;

	case EFUSE_IOC_WRITE:
		ret = cvi_efuse_write(data.addr, data.value);
		if (ret < 0)
			return ret;
		break;

	default:
		return -ENOTTY;
	}

	return 0;
}


static const struct file_operations efuse_fops = {
	.owner = THIS_MODULE,
	.open = efuse_open,
	.release = efuse_release,
	.read = efuse_read,
	.write = efuse_write,
	.unlocked_ioctl = efuse_ioctl,
};

static int __init cv181x_efuse_init(void)
{
	int ret;

	efuse_base = ioremap(EFUSE_BASE, 0x1000);
	if (efuse_base == NULL)
		return -ENOMEM;

	efuse_clk = clk_get_sys(NULL, "clk_efuse");
	if (IS_ERR(efuse_clk)) {
		pr_err("%s: efuse clock not found %ld\n", __func__,
		       PTR_ERR(efuse_clk));
		return PTR_ERR(efuse_clk);
	}

	// Dynamically allocate a device number
	ret = alloc_chrdev_region(&efuse_dev, 0, 1, "efuse");
	if (ret < 0) {
		pr_err("Failed to allocate device number\n");
		return ret;
	}

	// Initialize the cdev structure and add it to the kernel
	cdev_init(&efuse_cdev, &efuse_fops);
	ret = cdev_add(&efuse_cdev, efuse_dev, 1);
	if (ret < 0) {
		pr_err("Failed to add character device\n");
		unregister_chrdev_region(efuse_dev, 1);
		return ret;
	}

	// Create the device class
	efuse_class = class_create(THIS_MODULE, "efuse");
	if (IS_ERR(efuse_class)) {
		pr_err("Failed to create device class\n");
		cdev_del(&efuse_cdev);
		unregister_chrdev_region(efuse_dev, 1);
		return PTR_ERR(efuse_class);
	}

	// Create the device file
	if (IS_ERR(device_create(efuse_class, NULL, efuse_dev, NULL, "efuse"))) {
		pr_err("Failed to create device file\n");
		class_destroy(efuse_class);
		cdev_del(&efuse_cdev);
		unregister_chrdev_region(efuse_dev, 1);
		return PTR_ERR(efuse_class);
	}

	pr_info("Efuse device created successfully\n");
	return 0;
}

void __exit cv181x_efuse_exit(void)
{
	iounmap(efuse_base);

	device_destroy(efuse_class, efuse_dev);
	class_destroy(efuse_class);
	cdev_del(&efuse_cdev);
	unregister_chrdev_region(efuse_dev, 1);

	pr_info("Efuse device removed\n");
}

int64_t cvi_efuse_read_from_shadow(uint32_t addr)
{
	int64_t ret = -1;

	if (addr >= EFUSE_SIZE)
		return -EFAULT;

	if (addr % 4 != 0)
		return -EFAULT;

	ret = clk_prepare_enable(efuse_clk);
	if (ret) {
		pr_err("%s: clock failed to prepare+enable: %lld\n", __func__,
		       (long long)ret);
		return ret;
	}

	ret = mmio_read_32(EFUSE_SHADOW_REG + addr);
	clk_disable_unprepare(efuse_clk);

	return ret;
}
EXPORT_SYMBOL(cvi_efuse_read_from_shadow);

int cvi_efuse_write(uint32_t addr, uint32_t value)
{
#if 0
	int ret;
	VERBOSE("%s(): 0x%x = 0x%x\n", __func__, addr, value);

	if (addr >= EFUSE_SIZE)
		return -EFAULT;

	if (addr % 4 != 0)
		return -EFAULT;

	ret = clk_prepare_enable(efuse_clk);
	if (ret) {
		pr_err("%s: clock failed to prepare+enable: %lld\n", __func__,
		       (long long)ret);
		return ret;
	}

	ret = cvi_efuse_write_word(addr / 4, value);
	VERBOSE("%s(): ret=%d\n", __func__, ret);

	cvi_efuse_power_on(1);
	cvi_efuse_refresh();
	cvi_efuse_wait_for_ready();

	clk_disable_unprepare(efuse_clk);

	return ret;
#else
	return -1;
#endif
}
EXPORT_SYMBOL(cvi_efuse_write);

int cvi_efuse_read_buf(u32 addr, void *buf, size_t buf_size)
{
	int64_t ret = -1;
	int i;

	if (!buf)
		return -EFAULT;

	if (buf_size > EFUSE_SIZE)
		buf_size = EFUSE_SIZE;

	memset(buf, 0, buf_size);

	for (i = 0; i < buf_size; i += 4) {
		ret = cvi_efuse_read_from_shadow(addr + i);
		if (ret < 0)
			return ret;

		if (ret > 0) {
			u32 v = ret;

			memcpy(buf + i, &v, sizeof(v));
		}
	}

	return buf_size;
}
EXPORT_SYMBOL(cvi_efuse_read_buf);



module_init(cv181x_efuse_init);
module_exit(cv181x_efuse_exit);

MODULE_AUTHOR("leon.liao@cvitek.com");
MODULE_DESCRIPTION("cv180x efuse driver");
MODULE_LICENSE("GPL");

