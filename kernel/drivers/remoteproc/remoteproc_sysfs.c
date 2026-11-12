// SPDX-License-Identifier: GPL-2.0-only
/*
 * Remote Processor Framework
 */

#include <linux/remoteproc.h>
#include <linux/slab.h>

#include "remoteproc_internal.h"
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
#include "hobot_remoteproc.h"
#endif

#define to_rproc(d) container_of(d, struct rproc, dev)
#define SBL_VERSION_FORMAT_LENGTH (39u)
#define MCU_VERSION_FORMAT_LENGTH (55u)
#define VDSP_VERSION_FORMAT_LENGTH (32u)

static ssize_t recovery_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rproc *rproc = to_rproc(dev);

	return sysfs_emit(buf, "%s", rproc->recovery_disabled ? "disabled\n" : "enabled\n");
}

/*
 * By writing to the 'recovery' sysfs entry, we control the behavior of the
 * recovery mechanism dynamically. The default value of this entry is "enabled".
 *
 * The 'recovery' sysfs entry supports these commands:
 *
 * enabled:	When enabled, the remote processor will be automatically
 *		recovered whenever it crashes. Moreover, if the remote
 *		processor crashes while recovery is disabled, it will
 *		be automatically recovered too as soon as recovery is enabled.
 *
 * disabled:	When disabled, a remote processor will remain in a crashed
 *		state if it crashes. This is useful for debugging purposes;
 *		without it, debugging a crash is substantially harder.
 *
 * recover:	This function will trigger an immediate recovery if the
 *		remote processor is in a crashed state, without changing
 *		or checking the recovery state (enabled/disabled).
 *		This is useful during debugging sessions, when one expects
 *		additional crashes to happen after enabling recovery. In this
 *		case, enabling recovery will make it hard to debug subsequent
 *		crashes, so it's recommended to keep recovery disabled, and
 *		instead use the "recover" command as needed.
 */
static ssize_t recovery_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rproc *rproc = to_rproc(dev);

	if (sysfs_streq(buf, "enabled")) {
		/* change the flag and begin the recovery process if needed */
		rproc->recovery_disabled = false;
		rproc_trigger_recovery(rproc);
	} else if (sysfs_streq(buf, "disabled")) {
		rproc->recovery_disabled = true;
	} else if (sysfs_streq(buf, "recover")) {
		/* begin the recovery process without changing the flag */
		rproc_trigger_recovery(rproc);
	} else {
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_RW(recovery);

/*
 * A coredump-configuration-to-string lookup table, for exposing a
 * human readable configuration via sysfs. Always keep in sync with
 * enum rproc_coredump_mechanism
 */
static const char * const rproc_coredump_str[] = {
	[RPROC_COREDUMP_DISABLED]	= "disabled",
	[RPROC_COREDUMP_ENABLED]	= "enabled",
	[RPROC_COREDUMP_INLINE]		= "inline",
};

/* Expose the current coredump configuration via debugfs */
static ssize_t coredump_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rproc *rproc = to_rproc(dev);

	return sysfs_emit(buf, "%s\n", rproc_coredump_str[rproc->dump_conf]);
}

/*
 * By writing to the 'coredump' sysfs entry, we control the behavior of the
 * coredump mechanism dynamically. The default value of this entry is "default".
 *
 * The 'coredump' sysfs entry supports these commands:
 *
 * disabled:	This is the default coredump mechanism. Recovery will proceed
 *		without collecting any dump.
 *
 * default:	When the remoteproc crashes the entire coredump will be
 *		copied to a separate buffer and exposed to userspace.
 *
 * inline:	The coredump will not be copied to a separate buffer and the
 *		recovery process will have to wait until data is read by
 *		userspace. But this avoid usage of extra memory.
 */
static ssize_t coredump_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rproc *rproc = to_rproc(dev);

	if (rproc->state == RPROC_CRASHED) {
		dev_err(&rproc->dev, "can't change coredump configuration\n");
		return -EBUSY;
	}

	if (sysfs_streq(buf, "disabled")) {
		rproc->dump_conf = RPROC_COREDUMP_DISABLED;
	} else if (sysfs_streq(buf, "enabled")) {
		rproc->dump_conf = RPROC_COREDUMP_ENABLED;
	} else if (sysfs_streq(buf, "inline")) {
		rproc->dump_conf = RPROC_COREDUMP_INLINE;
	} else {
		dev_err(&rproc->dev, "Invalid coredump configuration\n");
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_RW(coredump);

/* Expose the loaded / running firmware name via sysfs */
static ssize_t firmware_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct rproc *rproc = to_rproc(dev);
	const char *firmware = rproc->firmware;

	/*
	 * If the remote processor has been started by an external
	 * entity we have no idea of what image it is running.  As such
	 * simply display a generic string rather then rproc->firmware.
	 */
	if (rproc->state == RPROC_ATTACHED)
		firmware = "unknown";

	return sprintf(buf, "%s\n", firmware);
}

/* Change firmware name via sysfs */
static ssize_t firmware_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rproc *rproc = to_rproc(dev);
	int err;

	err = rproc_set_firmware(rproc, buf);

	return err ? err : count;
}
static DEVICE_ATTR_RW(firmware);

/*
 * A state-to-string lookup table, for exposing a human readable state
 * via sysfs. Always keep in sync with enum rproc_state
 */
static const char * const rproc_state_string[] = {
	[RPROC_OFFLINE]		= "offline",
	[RPROC_SUSPENDED]	= "suspended",
	[RPROC_RUNNING]		= "running",
	[RPROC_CRASHED]		= "crashed",
	[RPROC_DELETED]		= "deleted",
	[RPROC_ATTACHED]	= "attached",
	[RPROC_DETACHED]	= "detached",
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
	[RPROC_RUNNING_INDEPENDENT] = "running_independent",
#endif
	[RPROC_LAST]		= "invalid",
};

/* Expose the state of the remote processor via sysfs */
static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct rproc *rproc = to_rproc(dev);
	unsigned int state;

	state = rproc->state > RPROC_LAST ? RPROC_LAST : rproc->state;
	return sprintf(buf, "%s\n", rproc_state_string[state]);
}

/* Change remote processor state via sysfs */
static ssize_t state_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rproc *rproc = to_rproc(dev);
	int ret = 0;

	if (sysfs_streq(buf, "start")) {
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
		if (rproc->state == RPROC_RUNNING ||
		rproc->state == RPROC_RUNNING_INDEPENDENT)
			return -EBUSY;
#endif
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
		ret = hobot_remoteproc_boot_vdsp((rproc->index - 1), (-1), NULL);
#else
		ret = rproc_boot(rproc);
#endif
		if (ret)
			dev_err(&rproc->dev, "Boot failed: %d\n", ret);
	} else if (sysfs_streq(buf, "stop")) {
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
		rproc->register_virtio = 0;
		if (rproc->state != RPROC_RUNNING &&
		rproc->state != RPROC_RUNNING_INDEPENDENT)
			return -EINVAL;
#endif
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
		ret = hobot_remoteproc_shutdown_vdsp(rproc->index - 1);
#else
		ret = rproc_shutdown(rproc);
#endif
		if (ret)
			dev_err(&rproc->dev, "shutdown failed: %d\n", ret);
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
	} else if (sysfs_streq(buf, "stopbin")) {
		rproc_shutdown_mcu(rproc);
	} else if (sysfs_streq(buf, "start_dynamic_bin")) {
		ret = rproc_boot_testcase(rproc);
		if (ret)
			dev_err(&rproc->dev, "Register virtio failed: %d\n", ret);
	} else if(sysfs_streq(buf, "mcu_reset")){
		rproc_mcu_reset(rproc);
	} else if(sysfs_streq(buf, "start_mcu_bin")){
		ret = rproc_boot_mcu(rproc);
		if (ret)
			dev_err(&rproc->dev, "Register virtio failed: %d\n", ret);
#endif
	} else if (sysfs_streq(buf, "detach")) {
		ret = rproc_detach(rproc);
	} else {
		dev_err(&rproc->dev, "Unrecognised option: %s\n", buf);
		ret = -EINVAL;
	}
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(state);

/* Expose the name of the remote processor via sysfs */
static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct rproc *rproc = to_rproc(dev);

	return sprintf(buf, "%s\n", rproc->name);
}
static DEVICE_ATTR_RO(name);

static umode_t rproc_is_visible(struct kobject *kobj, struct attribute *attr,
				int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct rproc *rproc = to_rproc(dev);
	umode_t mode = attr->mode;

	if (rproc->sysfs_read_only && (attr == &dev_attr_recovery.attr ||
				       attr == &dev_attr_firmware.attr ||
				       attr == &dev_attr_state.attr ||
				       attr == &dev_attr_coredump.attr))
		mode = 0444;

	return mode;
}

#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)|| defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
static ssize_t log_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct rproc *rproc = to_rproc(dev);
	int ret = 0;
	if (rproc == NULL || rproc->ops == NULL || rproc->ops->log == NULL) {
		pr_info("not support log show\n");
	} else {
		ret = rproc->ops->log(dev, attr, buf);
	}
	return ret;
}
static ssize_t log_store(struct device *dev,
struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(log);
#endif

#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
static ssize_t shell_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return 0;
}

static ssize_t shell_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rproc *rproc = to_rproc(dev);
	int ret = 0;
	if (rproc == NULL || rproc->ops == NULL || rproc->ops->shell == NULL) {
		pr_info("not support shell store\n");
	} else {
		ret = rproc->ops->shell(dev, attr, buf, count);
	}
	return ret;
}
static DEVICE_ATTR_RW(shell);

static ssize_t sbl_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct rproc *rproc = to_rproc(dev);

	return snprintf(buf, MCU_VERSION_LEN * 2 + SBL_VERSION_FORMAT_LENGTH,
		"SBL Build time = %s\nSBL Commoit Info = %s\n",
		rproc->sbl_version.build_time, rproc->sbl_version.commoit_info);
}

static ssize_t sbl_version_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(sbl_version);

static ssize_t mcu_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct rproc *rproc = to_rproc(dev);

	return snprintf(buf, sizeof(rproc->mcu_version) + MCU_VERSION_FORMAT_LENGTH,
		"MCU Board type = %s\nMCU Build time = %s\nMCU Commoit Info = %s\n",
		rproc->mcu_version.version, rproc->mcu_version.build_time, rproc->mcu_version.commoit_info);
}

static ssize_t mcu_version_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(mcu_version);
#endif

#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
static ssize_t version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct rproc *rproc = to_rproc(dev);

	return snprintf(buf, sizeof(rproc->version) + VDSP_VERSION_FORMAT_LENGTH,
		"version = %s\ncompile_time = %s\ngit_hash_id = %s\n",
		rproc->version.version, rproc->version.compile_time, rproc->version.git_hash_id);
}

static ssize_t version_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(version);


static ssize_t timesync_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return 0;
}

/* Change firmware name via sysfs */
static ssize_t timesync_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rproc *rproc = to_rproc(dev);
	uint32_t *p=(uint32_t *)buf;
	if (rproc == NULL || rproc->ops == NULL || rproc->ops->timesync == NULL) {
		pr_info("not support timesync store\n");
	} else {
		rproc->ops->timesync(dev,*p,*(p+1),*(p+2));
	}
	return count;
}
static DEVICE_ATTR_RW(timesync);
#endif

static struct attribute *rproc_attrs[] = {
	&dev_attr_coredump.attr,
	&dev_attr_recovery.attr,
	&dev_attr_firmware.attr,
	&dev_attr_state.attr,
	&dev_attr_name.attr,
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	&dev_attr_log.attr,
#endif
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
	&dev_attr_shell.attr,
	&dev_attr_mcu_version.attr,
	&dev_attr_sbl_version.attr,
#endif
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	&dev_attr_version.attr,
	&dev_attr_timesync.attr,
#endif
	NULL
};

static const struct attribute_group rproc_devgroup = {
	.attrs = rproc_attrs,
	.is_visible = rproc_is_visible,
};

static const struct attribute_group *rproc_devgroups[] = {
	&rproc_devgroup,
	NULL
};

struct class rproc_class = {
	.name		= "remoteproc",
	.dev_groups	= rproc_devgroups,
};

int __init rproc_init_sysfs(void)
{
	/* create remoteproc device class for sysfs */
	int err = class_register(&rproc_class);

	if (err)
		pr_err("remoteproc: unable to register class\n");
	return err;
}

void __exit rproc_exit_sysfs(void)
{
	class_unregister(&rproc_class);
}
