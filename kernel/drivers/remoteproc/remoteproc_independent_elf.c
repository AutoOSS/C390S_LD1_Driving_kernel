/*
 *			 COPYRIGHT NOTICE
 *		 Copyright 2021 Horizon Robotics, Inc.
 *			 All rights reserved.
 */

#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/elf.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/remoteproc.h>
#include <linux/mutex.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include "remoteproc_internal.h"

#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
#define MODULE_FIRMWARE_NAME "Mcore_independent.elf"
#define TARGET_FIRMWARE_NAME "Mcore_target_independent.elf"
#define TARGET_FIRMWARE_PATH "/tmp/Mcore_target_independent.elf"

struct dts_rsc_devmem {
	uint32_t da;
	uint32_t pa;
	uint32_t len;
};

struct dts_rsc_vdev_vring {
	uint32_t da;
	uint32_t align;
	uint32_t num;
	uint32_t vring_size;
};

struct dts_rsc_vdev {
	uint32_t buffer_size;
	struct dts_rsc_vdev_vring vring[2];
};

struct dts_rsc_rproc_mem {
	uint32_t da;
	uint32_t pa;
	uint32_t len;
};

struct dts_rsc_version {
	uint8_t version[VERSION_LEN];
	uint8_t compile_time[COMPILE_TIME_LEN];
	uint8_t git_hash_id[GIT_HASH_ID_LEN];
};

struct global_data {
	struct platform_device *pdev;
	uint8_t *data;
	size_t size;
	struct resource_table *table_ptr;
	int32_t tablesz;
	struct dts_rsc_devmem itcm_devmem;
	struct dts_rsc_devmem dtcm_devmem;
	struct dts_rsc_vdev vdev;
	struct dts_rsc_rproc_mem rproc_mem;
	struct dts_rsc_version rproc_version;
};
static struct global_data gd_global;

static const struct of_device_id remoteproc_independent_dt_ids[] = {
	{.compatible = "hobot,remoteproc-independent",},
        {/* sentinel */}
};

typedef int32_t (*rproc_ind_handle_rsc_t)(
const struct global_data *gd, void *rsc, int32_t offset, int32_t avail);

static int32_t rproc_ind_handle_devmem(const struct global_data *gd,
struct fw_rsc_devmem *rsc, int32_t offset, int32_t avail) /* PRQA S 3206 */
{
	struct device *dev = &(gd->pdev->dev);

	if ((int32_t)sizeof(*rsc) > avail) {
		dev_err(dev, "devmem rsc is truncated\n");
		return -EINVAL;
	}

	/* make sure reserved bytes are zeroes */
	if (rsc->reserved != 0u) {
		dev_err(dev, "devmem rsc has non zero reserved bytes\n");
		return -EINVAL;
	}

	dev_info(dev, "[before]%s: da = 0x%x pa = 0x%x len = 0x%x\n",
	rsc->name, rsc->da, rsc->pa, rsc->len);

	if (strcmp(rsc->name, "itcm devmem") == 0) {
		rsc->da = gd->itcm_devmem.da;
		rsc->pa = gd->itcm_devmem.pa;
		rsc->len = gd->itcm_devmem.len;
	} else {
		rsc->da = gd->dtcm_devmem.da;
		rsc->pa = gd->dtcm_devmem.pa;
		rsc->len = gd->dtcm_devmem.len;
	}

	dev_info(dev, "[after]%s: da = 0x%x pa = 0x%x len = 0x%x\n",
	rsc->name, rsc->da, rsc->pa, rsc->len);

	return 0;
}

static int32_t rproc_ind_handle_vdev(const struct global_data *gd,
struct fw_rsc_vdev *rsc, int32_t offset, int32_t avail) /* PRQA S 3206 */
{
	struct device *dev = &(gd->pdev->dev);
	uint8_t i = 0;
	struct fw_rsc_vdev_vring *vring = NULL;

	/* make sure resource isn't truncated */
	if ((sizeof(*rsc) + (rsc->num_of_vrings * sizeof(struct fw_rsc_vdev_vring))
			+ rsc->config_len) > (uint32_t)avail) {
		dev_err(dev, "vdev rsc is truncated\n");
		return -EINVAL;
	}

	/* make sure reserved bytes are zeroes */
	if ((rsc->reserved[0] != 0u) || (rsc->reserved[1] != 0u)) {
		dev_err(dev, "vdev rsc has non zero reserved bytes\n");
		return -EINVAL;
	}

	dev_info(dev, "[before]vdev rsc: id %d, dfeatures 0x%x, cfg len %d, %d vrings, buffer_size %d\n",
	rsc->id, rsc->dfeatures, rsc->config_len, rsc->num_of_vrings, rsc->buffer_size);

	/* we currently support only two vrings per rvdev */
	if (rsc->num_of_vrings > (uint8_t)RVDEV_NUM_VRINGS) {
		dev_err(dev, "too many vrings: %d\n", rsc->num_of_vrings);
		return -EINVAL;
	}

	rsc->buffer_size = gd->vdev.buffer_size;

	dev_info(dev, "[after]vdev rsc: id %d, dfeatures 0x%x, cfg len %d, %d vrings, buffer_size %d\n",
	rsc->id, rsc->dfeatures, rsc->config_len, rsc->num_of_vrings, rsc->buffer_size);

	/* parse the vrings */
	for (i = 0; i < rsc->num_of_vrings; i++) {
		vring = &rsc->vring[i];
		dev_info(dev, "[before]vdev rsc: vring %d da 0x%x qsz %d align %d vring_size %d\n",
		i, vring->da, vring->num, vring->align, vring->vring_size);

		vring->da = gd->vdev.vring[i].da;
		vring->align = gd->vdev.vring[i].align;
		vring->num = gd->vdev.vring[i].num;
		vring->vring_size = gd->vdev.vring[i].vring_size;

		dev_info(dev, "[after]vdev rsc: vring %d da 0x%x qsz %d align %d vring_size %d\n",
		i, vring->da, vring->num, vring->align, vring->vring_size);
	}

	return 0;
}

static int32_t rproc_ind_handle_rproc_mem(const struct global_data *gd,
struct fw_rsc_rproc_mem *rsc, int32_t offset, int32_t avail) /* PRQA S 3206 */
{
	struct device *dev = &(gd->pdev->dev);

	if ((int32_t)sizeof(*rsc) > avail) {
		dev_err(dev, "rproc_mem rsc is truncated\n");
		return -EINVAL;
	}

	if (rsc->pa == FW_RSC_ADDR_ANY) { /* PRQA S 1833, 2890 */
		dev_err(dev, "not able to declare rproc mem, pa is 0x%x\n",
		rsc->pa);
		return -EINVAL;
	}

	dev_info(dev, "[before]%s: da = 0x%x pa = 0x%x len = 0x%x\n", __func__, rsc->da, rsc->pa, rsc->len);

	rsc->da = gd->rproc_mem.da;
	rsc->pa = gd->rproc_mem.pa;
	rsc->len = gd->rproc_mem.len;

	dev_info(dev, "[after]%s: da = 0x%x pa = 0x%x len = 0x%x\n", __func__, rsc->da, rsc->pa, rsc->len);

	return 0;
}

static int32_t rproc_ind_handle_rproc_version(const struct global_data *gd,
struct fw_rsc_version *rsc, int32_t offset, int32_t avail) /* PRQA S 3206 */
{
	struct device *dev = &(gd->pdev->dev);

	if ((int32_t)sizeof(*rsc) > avail) {
		dev_err(dev, "rproc_version rsc is truncated\n");
		return -EINVAL;
	}

	dev_info(dev, "[before]%s: version = %s compile_time = %s git_hash_id = %s\n",
	__func__, rsc->version, rsc->compile_time, rsc->git_hash_id);

	strncpy(rsc->version, gd->rproc_version.version, VERSION_LEN - 1);
	rsc->version[VERSION_LEN - 1] = '\0';
	strncpy(rsc->compile_time, gd->rproc_version.compile_time, COMPILE_TIME_LEN - 1);
	rsc->compile_time[COMPILE_TIME_LEN - 1] = '\0';
	strncpy(rsc->git_hash_id, gd->rproc_version.git_hash_id, GIT_HASH_ID_LEN - 1);
	rsc->git_hash_id[GIT_HASH_ID_LEN - 1] = '\0';

	dev_info(dev, "[after]%s: version = %s compile_time = %s git_hash_id = %s\n",
	__func__, rsc->version, rsc->compile_time, rsc->git_hash_id);

	return 0;
}


static rproc_ind_handle_rsc_t
rproc_independent_handlers[RSC_LAST] = {
	[RSC_DEVMEM] = (rproc_ind_handle_rsc_t)rproc_ind_handle_devmem, /* PRQA S 0313 */
	[RSC_VDEV] = (rproc_ind_handle_rsc_t)rproc_ind_handle_vdev, /* PRQA S 0313 */
	[RSC_RPROC_MEM] = (rproc_ind_handle_rsc_t)rproc_ind_handle_rproc_mem, /* PRQA S 0313 */
	[RSC_VERSION] = (rproc_ind_handle_rsc_t)rproc_ind_handle_rproc_version, /* PRQA S 0313 */
}; /* PRQA S 0691 */

static int32_t rproc_ind_handle_resources(const struct global_data *gd, int32_t tablesz,
const rproc_ind_handle_rsc_t handlers[RSC_LAST])
{
	struct device *dev = &(gd->pdev->dev);
	rproc_ind_handle_rsc_t handler = NULL;
	int32_t ret = 0;
	uint32_t i = 0;

	for (i = 0; i < gd->table_ptr->num; i++) {
		uint32_t offset = gd->table_ptr->offset[i];
		struct fw_rsc_hdr *hdr = (struct fw_rsc_hdr *)((void *)gd->table_ptr + offset); /* PRQA S 0497 */
		int32_t avail = tablesz - offset - sizeof(*hdr); /* PRQA S 1820, 4446, 1891 */
		void *rsc = (void *)((void *)hdr + sizeof(*hdr)); /* PRQA S 0497 */

		/* make sure table isn't truncated */
		if (avail < 0) {
			dev_err(dev, "rsc table is truncated\n");
			return -EINVAL;
		}

		dev_dbg(dev, "rsc: type %d\n", hdr->type); /* PRQA S 4543, 4558, 3344, 4542, 3432, 1861, 4403 */

		if (hdr->type >= RSC_LAST) { /* PRQA S 1880 */
			dev_warn(dev, "unsupported resource %d\n", hdr->type);
			continue;
		}

		handler = handlers[hdr->type];
		if (handler == NULL)
			continue;

		ret = handler(gd, rsc, offset + sizeof(*hdr), avail); /* PRQA S 4446 */
		if (ret != 0)
			break;
	}

	return ret;
}

static int find_rsc_table(const struct device *dev, struct elf32_shdr *shdr,
struct elf32_hdr *ehdr, size_t fw_size)
{
	uint32_t i = 0;
	const char *name_table = NULL;
	struct resource_table *table = NULL;
	const u8 *elf_data = (u8 *)ehdr;

	/* look for the resource table and handle it */
	shdr = (struct elf32_shdr *)(elf_data + ehdr->e_shoff); /* PRQA S 0488, 0311, 3305, 0310 */
	name_table = elf_data + shdr[ehdr->e_shstrndx].sh_offset; /* PRQA S 0488 */

	for (i = 0; i < ehdr->e_shnum; i++, shdr++) { /* PRQA S 3387, 3418, 1485, 1852, 2463 */
		uint32_t size = shdr->sh_size;
		uint32_t offset = shdr->sh_offset;

		if (strcmp(name_table + shdr->sh_name, ".resource_table") != 0) /* PRQA S 0488 */
			continue;

		table = (struct resource_table *)(elf_data + offset); /* PRQA S 0310, 0488, 0311, 3305 */

		/* make sure we have the entire table */
		if (((offset + size) > fw_size) || ((offset + size) < size)) { /* PRQA S 1891 */
			dev_err(dev, "resource table truncated\n");
			return -EINVAL;
		}

		/* make sure table has at least the header */
		if (sizeof(struct resource_table) > size) {
			dev_err(dev, "header-less resource table\n");
			return -EINVAL;
		}

		/* we don't support any version beyond the first */
		if (table->ver != 1u) {
			dev_err(dev, "unsupported fw ver: %d\n", table->ver);
			return -EINVAL;
		}

		/* make sure reserved bytes are zeroes */
		if ((table->reserved[0] != 0u) || (table->reserved[1] != 0u)) {
			dev_err(dev, "non zero reserved bytes\n");
			return -EINVAL;
		}

		/* make sure the offsets array isn't truncated */
		if (((table->num * sizeof(table->offset[0])) +
				sizeof(struct resource_table)) > size) {
			dev_err(dev, "resource table incomplete\n");
			return -EINVAL;
		}

		break;
	}

	if(i == ehdr->e_shnum){
		dev_err(dev, "can't find in table\n");
		return -EINVAL;
	}

	return 0;
}

int32_t rproc_independent_read_firmware(void) {
	const struct firmware *fw = NULL;
	struct device *dev = &(gd_global.pdev->dev);
	int32_t ret = 0;
	struct elf32_hdr *ehdr = NULL;
	struct elf32_shdr *shdr = NULL;

	ret = request_firmware(&fw, MODULE_FIRMWARE_NAME, dev);
	if (ret < 0) {
		dev_err(dev, "request_firmware error\n");
		return -ENODEV;
	}

	if (fw->size <= 0u) {
		release_firmware(fw);
		return -EINVAL;
	}

	if (gd_global.data != NULL) {
		kfree((void *)gd_global.data);
		gd_global.data = NULL;
	}

	gd_global.data = (uint8_t *)kmalloc(fw->size, GFP_ATOMIC);
	if (gd_global.data == NULL) {
		dev_err(dev, "malloc firmware buffer error\n");
		release_firmware(fw);
		return -ENOMEM;
	}
	(void)memcpy((void *)gd_global.data, (const void *)fw->data, fw->size);
	gd_global.size = fw->size;

	ehdr = (struct elf32_hdr *)gd_global.data; /* PRQA S 3305, 0310 */
	ret = find_rsc_table(dev, shdr, ehdr, gd_global.size);
	if (ret < 0) {
		dev_err(dev, "find_rsc_table error\n");
		kfree((void *)gd_global.data);
		gd_global.data = NULL;
		release_firmware(fw);
		return -EFAULT;
	}
	gd_global.table_ptr = (struct resource_table *)(gd_global.data + shdr->sh_offset); /* PRQA S 0488, 0310, 3305 */
	gd_global.tablesz = shdr->sh_size; /* PRQA S 4446 */

	dev_info(dev, "table_ptr->ver = %d\n", gd_global.table_ptr->ver);
	dev_info(dev, "table_ptr->num = %d\n", gd_global.table_ptr->num);
	dev_info(dev, "tablesz = %d\n", gd_global.tablesz);

	ret = rproc_ind_handle_resources(&gd_global, gd_global.tablesz, rproc_independent_handlers);
	if (ret != 0) {
		dev_err(dev, "rproc_ind_handle_resources fail[%d]\n", ret);
		kfree((void *)gd_global.data);
		gd_global.data = NULL;
		release_firmware(fw);
		return -ENOSYS;
	}

	return 0;
}
EXPORT_SYMBOL(rproc_independent_read_firmware); /* PRQA S 3449, 0305, 3451, 1020 */

#define FIRMWARE_BUF_SIZE (strlen(TARGET_FIRMWARE_NAME) + 2)
int32_t rproc_independent_set_target_fw(struct rproc *rproc)
{
	int32_t ret = 0;
	struct device *dev = &(gd_global.pdev->dev);
	char *str = NULL;

	if (rproc == NULL) {
		dev_err(dev, "rproc invalid\n");
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&rproc->lock);
	if (ret != 0) {
		dev_err(dev, "can't lock rproc %s: %d\n", rproc->name, ret);
		return -EINVAL;
	}

	if (rproc->state != RPROC_OFFLINE) { /* PRQA S 1881 */
		dev_err(dev, "can't change firmware while running\n");
		mutex_unlock(&rproc->lock);
		return -EBUSY;
	}

	str = (char *)kmalloc(FIRMWARE_BUF_SIZE, GFP_ATOMIC); /* PRQA S 1840 */
	if (str == NULL) {
		dev_err(dev, "kmalloc string error\n");
		mutex_unlock(&rproc->lock);
		return -ENOMEM;
	}

	(void)strncpy(str, TARGET_FIRMWARE_NAME, FIRMWARE_BUF_SIZE - 1); /* PRQA S 1840 */
	str[FIRMWARE_BUF_SIZE - 1] = '\0'; /* PRQA S 1840 */
	kfree((void *)rproc->firmware);
	rproc->firmware = str;
	mutex_unlock(&rproc->lock);
	return ret;
}
EXPORT_SYMBOL(rproc_independent_set_target_fw); /* PRQA S 3449, 0305, 3451, 1020 */

int32_t rproc_independent_write_firmware(void) {
	int32_t ret = 0;
	struct file *fp = NULL;
	struct device *dev = &(gd_global.pdev->dev);
	loff_t pos = 0;

	fp = filp_open(TARGET_FIRMWARE_PATH, O_CREAT | O_TRUNC | /* PRQA S 4542 */
	O_WRONLY, S_IWUSR | S_IRUSR); /* PRQA S 4542 */
	if (IS_ERR(fp)) { /* PRQA S 0315 */
		dev_err(dev, "filp_open error\n");
		ret = PTR_ERR(fp); /* PRQA S 0315, 4460 */
		return -EAGAIN;
	}

	ret = (int32_t)kernel_write(fp, gd_global.data, gd_global.size, &pos);
	if (ret < 0) {
		dev_err(dev, "file write error\n");
		(void)filp_close(fp, NULL);
		return -EAGAIN;
	}

	(void)filp_close(fp, NULL);
	return 0;
}
EXPORT_SYMBOL(rproc_independent_write_firmware); /* PRQA S 3449, 0305, 3451, 1020 */

static int32_t get_devmem_property(struct global_data *gd) {
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = of_property_read_u32(pdev->dev.of_node, "itcm_da",
	&(gd->itcm_devmem.da));
	if (ret != 0) {
		dev_err(&pdev->dev, "get itcm_da error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "itcm_pa",
	&(gd->itcm_devmem.pa));
	if (ret != 0) {
		dev_err(&pdev->dev, "get itcm_pa error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "itcm_len",
	&(gd->itcm_devmem.len));
	if (ret != 0) {
		dev_err(&pdev->dev, "get itcm_len error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "dtcm_da",
	&(gd->dtcm_devmem.da));
	if (ret != 0) {
		dev_err(&pdev->dev, "get dtcm_da error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "dtcm_pa",
	&(gd->dtcm_devmem.pa));
	if (ret != 0) {
		dev_err(&pdev->dev, "get dtcm_pa error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "dtcm_len",
	&(gd->dtcm_devmem.len));
	if (ret != 0) {
		dev_err(&pdev->dev, "get dtcm_len error\n");
		return -EINVAL;
	}

	return 0;
}

static int32_t get_vdev_property_vring0(struct global_data *gd)
{
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = of_property_read_u32(pdev->dev.of_node, "vring0_da",
	&(gd->vdev.vring[0].da));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring0_da error\n");
	return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "vring0_align",
	&(gd->vdev.vring[0].align));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring0_align error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "vring0_num",
	&(gd->vdev.vring[0].num));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring0_num error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "vring0_vring_size",
	&(gd->vdev.vring[0].vring_size));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring0_vring_size error\n");
		return -EINVAL;
	}

	return 0;
}

static int32_t get_vdev_property_vring1(struct global_data *gd)
{
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = of_property_read_u32(pdev->dev.of_node, "vring1_da",
	&(gd->vdev.vring[1].da));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring1_da error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "vring1_align",
	&(gd->vdev.vring[1].align));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring1_align error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "vring1_num",
	&(gd->vdev.vring[1].num));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring1_num error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "vring1_vring_size",
	&(gd->vdev.vring[1].vring_size));
	if (ret != 0) {
		dev_err(&pdev->dev, "get vring1_vring_size error\n");
		return -EINVAL;
	}

	return 0;
}

static int32_t get_vdev_property(struct global_data *gd)
{
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = of_property_read_u32(pdev->dev.of_node, "buffer_size",
	&(gd->vdev.buffer_size));
	if (ret != 0) {
		dev_err(&pdev->dev, "get buffer_size error\n");
		return -EINVAL;
	}

	ret = get_vdev_property_vring0(gd);
	if (ret != 0)
		return -EINVAL;

	ret = get_vdev_property_vring1(gd);
	if (ret != 0)
		return -EINVAL;

	return 0;
}

static int32_t get_rproc_mem_property(struct global_data *gd)
{
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = of_property_read_u32(pdev->dev.of_node, "rproc_mem_da",
	&(gd->rproc_mem.da));
	if (ret != 0) {
		dev_err(&pdev->dev, "get rproc_mem_da error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "rproc_mem_pa",
	&(gd->rproc_mem.pa));
	if (ret != 0) {
		dev_err(&pdev->dev, "get rproc_mem_pa error\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "rproc_mem_len",
	&(gd->rproc_mem.len));
	if (ret != 0) {
		dev_err(&pdev->dev, "get rproc_mem_len error\n");
		return -EINVAL;
	}

	return 0;
}

static int32_t get_rproc_version_property(struct global_data *gd)
{
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = of_property_read_variable_u8_array(pdev->dev.of_node, "version",
	gd->rproc_version.version, 0, VERSION_LEN);
	if (ret < 0) {
		dev_err(&pdev->dev, "get version error\n");
		return -EINVAL;
	}
	gd->rproc_version.version[VERSION_LEN - 1] = '\0';

	ret = of_property_read_variable_u8_array(pdev->dev.of_node, "compile_time",
	gd->rproc_version.compile_time, 0, COMPILE_TIME_LEN);
	if (ret < 0) {
		dev_err(&pdev->dev, "get compile_time error\n");
		return -EINVAL;
	}
	gd->rproc_version.compile_time[COMPILE_TIME_LEN - 1] = '\0';

	ret = of_property_read_variable_u8_array(pdev->dev.of_node, "git_hash_id",
	gd->rproc_version.git_hash_id, 0, GIT_HASH_ID_LEN);
	if (ret < 0) {
		dev_err(&pdev->dev, "get git_hash_id error\n");
		return -EINVAL;
	}
	gd->rproc_version.git_hash_id[GIT_HASH_ID_LEN - 1] = '\0';

	return 0;
}

static int32_t dts_init(struct global_data *gd)
{
	struct platform_device *pdev = gd->pdev;
	int32_t ret = 0;

	ret = get_devmem_property(gd);
	if (ret < 0) {
		dev_err(&pdev->dev, "get_devmem_property error\n");
		return -EINVAL;
	}

	ret = get_vdev_property(gd);
	if (ret < 0) {
		dev_err(&pdev->dev, "get_vdev_property error\n");
		return -EINVAL;
	}

	ret = get_rproc_mem_property(gd);
	if (ret < 0) {
		dev_err(&pdev->dev, "get_rproc_mem_property error\n");
		return -EINVAL;
	}

	ret = get_rproc_version_property(gd);
	if (ret < 0) {
		dev_err(&pdev->dev, "get_rproc_version_property error\n");
		return -EINVAL;
	}

	return 0;
}

static int32_t remoteproc_independent_probe(struct platform_device *pdev)
{
	int32_t ret = 0;

	gd_global.pdev = pdev;

	ret = dts_init(&gd_global);
	if (ret != 0)
		dev_err(&pdev->dev, "dts_init error\n");

	return ret;
}

static struct platform_driver remoteproc_independent_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "hobot-remoteproc-independent",
		.of_match_table = remoteproc_independent_dt_ids,
	},
	.probe = remoteproc_independent_probe,
};

static int32_t __init remoteproc_independent_init(void)
{
	int32_t ret = 0;

	ret = platform_driver_register(&remoteproc_independent_driver); /* PRQA S 3469 */
	if (ret != 0)
		(void)pr_err("[remoteproc_independent_elf]platform_driver_register error\n");  /* PRQA S 1035, 1036 */

	return ret;
}

MODULE_AUTHOR("Horizon Robotics, Inc"); /* PRQA S 0633, 3213 */
MODULE_DESCRIPTION("Remoteproc elf independent driver"); /* PRQA S 0633, 3213 */
MODULE_LICENSE("GPL_v2"); /* PRQA S 0633, 3213 */
module_init(remoteproc_independent_init); /* PRQA S 0605 */
#endif
