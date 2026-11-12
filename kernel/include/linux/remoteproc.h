/*
 * Remote Processor Framework
 *
 * Copyright(c) 2011 Texas Instruments, Inc.
 * Copyright(c) 2011 Google, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name Texas Instruments nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef REMOTEPROC_H
#define REMOTEPROC_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/virtio.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/of.h>

/**
 * struct resource_table - firmware resource table header
 * @ver: version number
 * @num: number of resource entries
 * @reserved: reserved (must be zero)
 * @offset: array of offsets pointing at the various resource entries
 *
 * A resource table is essentially a list of system resources required
 * by the remote processor. It may also include configuration entries.
 * If needed, the remote processor firmware should contain this table
 * as a dedicated ".resource_table" ELF section.
 *
 * Some resources entries are mere announcements, where the host is informed
 * of specific remoteproc configuration. Other entries require the host to
 * do something (e.g. allocate a system resource). Sometimes a negotiation
 * is expected, where the firmware requests a resource, and once allocated,
 * the host should provide back its details (e.g. address of an allocated
 * memory region).
 *
 * The header of the resource table, as expressed by this structure,
 * contains a version number (should we need to change this format in the
 * future), the number of available resource entries, and their offsets
 * in the table.
 *
 * Immediately following this header are the resource entries themselves,
 * each of which begins with a resource entry header (as described below).
 */
struct resource_table {
	u32 ver;
	u32 num;
	u32 reserved[2];
	u32 offset[];
} __packed;

/**
 * struct fw_rsc_hdr - firmware resource entry header
 * @type: resource type
 * @data: resource data
 *
 * Every resource entry begins with a 'struct fw_rsc_hdr' header providing
 * its @type. The content of the entry itself will immediately follow
 * this header, and it should be parsed according to the resource type.
 */
struct fw_rsc_hdr {
	u32 type;
	u8 data[];
} __packed;

/**
 * enum fw_resource_type - types of resource entries
 *
 * @RSC_CARVEOUT:   request for allocation of a physically contiguous
 *		    memory region.
 * @RSC_DEVMEM:     request to iommu_map a memory-based peripheral.
 * @RSC_TRACE:	    announces the availability of a trace buffer into which
 *		    the remote processor will be writing logs.
 * @RSC_VDEV:       declare support for a virtio device, and serve as its
 *		    virtio header.
 * @RSC_LAST:       just keep this one at the end of standard resources
 * @RSC_VENDOR_START:	start of the vendor specific resource types range
 * @RSC_VENDOR_END:	end of the vendor specific resource types range
 *
 * For more details regarding a specific resource type, please see its
 * dedicated structure below.
 *
 * Please note that these values are used as indices to the rproc_handle_rsc
 * lookup table, so please keep them sane. Moreover, @RSC_LAST is used to
 * check the validity of an index before the lookup table is accessed, so
 * please update it as needed.
 */
enum fw_resource_type {
	RSC_CARVEOUT		= 0,
	RSC_DEVMEM		= 1,
	RSC_TRACE		= 2,
	RSC_VDEV		= 3,
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	RSC_RPROC_MEM	= 4U,
	RSC_FW_CHKSUM	= 5U,
	RSC_VERSION		= 6U,
	RSC_LAST		= 7U,
#else
	RSC_LAST		= 4,
#endif
	RSC_VENDOR_START	= 128,
	RSC_VENDOR_END		= 512,
};

#define FW_RSC_ADDR_ANY (-1)

/**
 * struct fw_rsc_carveout - physically contiguous memory request
 * @da: device address
 * @pa: physical address
 * @len: length (in bytes)
 * @flags: iommu protection flags
 * @reserved: reserved (must be zero)
 * @name: human-readable name of the requested memory region
 *
 * This resource entry requests the host to allocate a physically contiguous
 * memory region.
 *
 * These request entries should precede other firmware resource entries,
 * as other entries might request placing other data objects inside
 * these memory regions (e.g. data/code segments, trace resource entries, ...).
 *
 * Allocating memory this way helps utilizing the reserved physical memory
 * (e.g. CMA) more efficiently, and also minimizes the number of TLB entries
 * needed to map it (in case @rproc is using an IOMMU). Reducing the TLB
 * pressure is important; it may have a substantial impact on performance.
 *
 * If the firmware is compiled with static addresses, then @da should specify
 * the expected device address of this memory region. If @da is set to
 * FW_RSC_ADDR_ANY, then the host will dynamically allocate it, and then
 * overwrite @da with the dynamically allocated address.
 *
 * We will always use @da to negotiate the device addresses, even if it
 * isn't using an iommu. In that case, though, it will obviously contain
 * physical addresses.
 *
 * Some remote processors needs to know the allocated physical address
 * even if they do use an iommu. This is needed, e.g., if they control
 * hardware accelerators which access the physical memory directly (this
 * is the case with OMAP4 for instance). In that case, the host will
 * overwrite @pa with the dynamically allocated physical address.
 * Generally we don't want to expose physical addresses if we don't have to
 * (remote processors are generally _not_ trusted), so we might want to
 * change this to happen _only_ when explicitly required by the hardware.
 *
 * @flags is used to provide IOMMU protection flags, and @name should
 * (optionally) contain a human readable name of this carveout region
 * (mainly for debugging purposes).
 */
struct fw_rsc_carveout {
	u32 da;
	u32 pa;
	u32 len;
	u32 flags;
	u32 reserved;
	u8 name[32];
} __packed;

/**
 * struct fw_rsc_devmem - iommu mapping request
 * @da: device address
 * @pa: physical address
 * @len: length (in bytes)
 * @flags: iommu protection flags
 * @reserved: reserved (must be zero)
 * @name: human-readable name of the requested region to be mapped
 *
 * This resource entry requests the host to iommu map a physically contiguous
 * memory region. This is needed in case the remote processor requires
 * access to certain memory-based peripherals; _never_ use it to access
 * regular memory.
 *
 * This is obviously only needed if the remote processor is accessing memory
 * via an iommu.
 *
 * @da should specify the required device address, @pa should specify
 * the physical address we want to map, @len should specify the size of
 * the mapping and @flags is the IOMMU protection flags. As always, @name may
 * (optionally) contain a human readable name of this mapping (mainly for
 * debugging purposes).
 *
 * Note: at this point we just "trust" those devmem entries to contain valid
 * physical addresses, but this isn't safe and will be changed: eventually we
 * want remoteproc implementations to provide us ranges of physical addresses
 * the firmware is allowed to request, and not allow firmwares to request
 * access to physical addresses that are outside those ranges.
 */
struct fw_rsc_devmem {
	u32 da;
	u32 pa;
	u32 len;
	u32 flags;
	u32 reserved;
	u8 name[32];
} __packed;

/**
 * struct fw_rsc_trace - trace buffer declaration
 * @da: device address
 * @len: length (in bytes)
 * @reserved: reserved (must be zero)
 * @name: human-readable name of the trace buffer
 *
 * This resource entry provides the host information about a trace buffer
 * into which the remote processor will write log messages.
 *
 * @da specifies the device address of the buffer, @len specifies
 * its size, and @name may contain a human readable name of the trace buffer.
 *
 * After booting the remote processor, the trace buffers are exposed to the
 * user via debugfs entries (called trace0, trace1, etc..).
 */
struct fw_rsc_trace {
	u32 da;
	u32 len;
	u32 reserved;
	u8 name[32];
} __packed;

/**
 * struct fw_rsc_vdev_vring - vring descriptor entry
 * @da: device address
 * @align: the alignment between the consumer and producer parts of the vring
 * @num: num of buffers supported by this vring (must be power of two)
 * @notifyid: a unique rproc-wide notify index for this vring. This notify
 * index is used when kicking a remote processor, to let it know that this
 * vring is triggered.
 * @pa: physical address
 *
 * This descriptor is not a resource entry by itself; it is part of the
 * vdev resource type (see below).
 *
 * Note that @da should either contain the device address where
 * the remote processor is expecting the vring, or indicate that
 * dynamically allocation of the vring's device address is supported.
 */
struct fw_rsc_vdev_vring {
	u32 da;
	u32 align;
	u32 num;
	u32 notifyid;
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	u32 vring_size;
#endif
	u32 pa;
} __packed;

/**
 * struct fw_rsc_vdev - virtio device header
 * @id: virtio device id (as in virtio_ids.h)
 * @notifyid: a unique rproc-wide notify index for this vdev. This notify
 * index is used when kicking a remote processor, to let it know that the
 * status/features of this vdev have changes.
 * @dfeatures: specifies the virtio device features supported by the firmware
 * @gfeatures: a place holder used by the host to write back the
 * negotiated features that are supported by both sides.
 * @config_len: the size of the virtio config space of this vdev. The config
 * space lies in the resource table immediate after this vdev header.
 * @status: a place holder where the host will indicate its virtio progress.
 * @num_of_vrings: indicates how many vrings are described in this vdev header
 * @reserved: reserved (must be zero)
 * @vring: an array of @num_of_vrings entries of 'struct fw_rsc_vdev_vring'.
 *
 * This resource is a virtio device header: it provides information about
 * the vdev, and is then used by the host and its peer remote processors
 * to negotiate and share certain virtio properties.
 *
 * By providing this resource entry, the firmware essentially asks remoteproc
 * to statically allocate a vdev upon registration of the rproc (dynamic vdev
 * allocation is not yet supported).
 *
 * Note:
 * 1. unlike virtualization systems, the term 'host' here means
 *    the Linux side which is running remoteproc to control the remote
 *    processors. We use the name 'gfeatures' to comply with virtio's terms,
 *    though there isn't really any virtualized guest OS here: it's the host
 *    which is responsible for negotiating the final features.
 *    Yeah, it's a bit confusing.
 *
 * 2. immediately following this structure is the virtio config space for
 *    this vdev (which is specific to the vdev; for more info, read the virtio
 *    spec). The size of the config space is specified by @config_len.
 */
struct fw_rsc_vdev {
	u32 id;
	u32 notifyid;
	u32 dfeatures;
	u32 gfeatures;
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	u32 buffer_size;
#endif
	u32 config_len;
	u8 status;
	u8 num_of_vrings;
	u8 reserved[2];
	struct fw_rsc_vdev_vring vring[];
} __packed;

#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
/**
 * struct fw_rsc_rproc_mem - remote processor memory
 * @da: device address
 * @pa: physical address
 * @len: length (in bytes)
 * @reserved: reserved (must be zero)
 *
 * This resource entry tells the host to the remote processor
 * memory that the host can be used as shared memory.
 *
 * These request entries should precede other shared resource entries
 * such as vdevs, vrings.
 */
struct fw_rsc_rproc_mem {
	u32 da;
	u32 pa;
	u32 len;
	u32 reserved;
} __packed;

/*
 * struct fw_rsc_fw_chksum - firmware checksum
 * @algo: algorithm to generate the cheksum
 * @chksum: checksum of the firmware loadable sections.
 *
 * This resource entry provides checksum for the firmware loadable sections.
 * It is used to check if the remote already runs with the expected firmware to
 * decide if it needs to start the remote if the remote is already running.
 */
struct fw_rsc_fw_chksum {
	u8 algo[16];
	u8 chksum[64];
} __packed;

#define VERSION_LEN      (48)
#define COMPILE_TIME_LEN (16)
#define GIT_HASH_ID_LEN  (40)

#define MCU_VERSION_LEN      (32)

struct fw_rsc_version {
	u8 version[VERSION_LEN];
	u8 compile_time[COMPILE_TIME_LEN];
	u8 git_hash_id[GIT_HASH_ID_LEN];
} __packed;
#endif

struct rproc;

/**
 * struct rproc_mem_entry - memory entry descriptor
 * @va:	virtual address
 * @is_iomem: io memory
 * @dma: dma address
 * @len: length, in bytes
 * @da: device address
 * @release: release associated memory
 * @priv: associated data
 * @name: associated memory region name (optional)
 * @node: list node
 * @rsc_offset: offset in resource table
 * @flags: iommu protection flags
 * @of_resm_idx: reserved memory phandle index
 * @alloc: specific memory allocator function
 */
struct rproc_mem_entry {
	void *va;
	bool is_iomem;
	dma_addr_t dma;
	size_t len;
	u32 da;
	void *priv;
	char name[32];
	struct list_head node;
	u32 rsc_offset;
	u32 flags;
	u32 of_resm_idx;
	int (*alloc)(struct rproc *rproc, struct rproc_mem_entry *mem);
	int (*release)(struct rproc *rproc, struct rproc_mem_entry *mem);
};

struct firmware;

/**
 * enum rsc_handling_status - return status of rproc_ops handle_rsc hook
 * @RSC_HANDLED:	resource was handled
 * @RSC_IGNORED:	resource was ignored
 */
enum rsc_handling_status {
	RSC_HANDLED	= 0,
	RSC_IGNORED	= 1,
};

/**
 * struct rproc_ops - platform-specific device handlers
 * @prepare:	prepare device for code loading
 * @unprepare:	unprepare device after stop
 * @start:	power on the device and boot it
 * @stop:	power off the device
 * @attach:	attach to a device that his already powered up
 * @detach:	detach from a device, leaving it powered up
 * @kick:	kick a virtqueue (virtqueue id given as a parameter)
 * @da_to_va:	optional platform hook to perform address translations
 * @parse_fw:	parse firmware to extract information (e.g. resource table)
 * @handle_rsc:	optional platform hook to handle vendor resources. Should return
 *		RSC_HANDLED if resource was handled, RSC_IGNORED if not handled
 *		and a negative value on error
 * @find_loaded_rsc_table: find the loaded resource table from firmware image
 * @get_loaded_rsc_table: get resource table installed in memory
 *			  by external entity
 * @load:		load firmware to memory, where the remote processor
 *			expects to find it
 * @sanity_check:	sanity check the fw image
 * @get_boot_addr:	get boot address to entry point specified in firmware
 * @panic:	optional callback to react to system panic, core will delay
 *		panic at least the returned number of milliseconds
 * @coredump:	  collect firmware dump after the subsystem is shutdown
 */
struct rproc_ops {
	int (*prepare)(struct rproc *rproc);
	int (*unprepare)(struct rproc *rproc);
	int (*start)(struct rproc *rproc);
	int (*stop)(struct rproc *rproc);
	int (*attach)(struct rproc *rproc);
	int (*detach)(struct rproc *rproc);
	void (*kick)(struct rproc *rproc, int vqid);
	void * (*da_to_va)(struct rproc *rproc, u64 da, size_t len, bool *is_iomem);
	int (*parse_fw)(struct rproc *rproc, const struct firmware *fw);
	int (*handle_rsc)(struct rproc *rproc, u32 rsc_type, void *rsc,
			  int offset, int avail);
	struct resource_table *(*find_loaded_rsc_table)(
				struct rproc *rproc, const struct firmware *fw);
	struct resource_table *(*get_loaded_rsc_table)(
				struct rproc *rproc, size_t *size);
	int (*load)(struct rproc *rproc, const struct firmware *fw);
	int (*sanity_check)(struct rproc *rproc, const struct firmware *fw);
	u64 (*get_boot_addr)(struct rproc *rproc, const struct firmware *fw);
	unsigned long (*panic)(struct rproc *rproc);
	void (*coredump)(struct rproc *rproc);
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	int (*get_chksum)(struct rproc *rproc, const struct firmware *fw,
			char *algo, u8 *chksum, int output_size);
	bool (*is_running)(struct rproc *rproc);
	int (*pre_load)(struct rproc *rproc);
	int (*pre_stop)(struct rproc *rproc);
	int (*log)(struct device *dev, struct device_attribute *attr, char *buf);
	int (*shell)(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count);
	void (*timesync)(struct device *dev,u32 second,u32 nanosec,u32 diff);
	int (*debug)(struct device *dev, struct device_attribute *attr,const char *buf);
	int (*live)(void);
#endif
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
	void (*reset)(struct rproc *rproc);
#endif
};

/**
 * enum rproc_state - remote processor states
 * @RPROC_OFFLINE:	device is powered off
 * @RPROC_SUSPENDED:	device is suspended; needs to be woken up to receive
 *			a message.
 * @RPROC_RUNNING:	device is up and running
 * @RPROC_CRASHED:	device has crashed; need to start recovery
 * @RPROC_DELETED:	device is deleted
 * @RPROC_ATTACHED:	device has been booted by another entity and the core
 *			has attached to it
 * @RPROC_DETACHED:	device has been booted by another entity and waiting
 *			for the core to attach to it
 * @RPROC_LAST:		just keep this one at the end
 *
 * Please note that the values of these states are used as indices
 * to rproc_state_string, a state-to-name lookup table,
 * so please keep the two synchronized. @RPROC_LAST is used to check
 * the validity of an index before the lookup table is accessed, so
 * please update it as needed too.
 */
enum rproc_state {
	RPROC_OFFLINE	= 0,
	RPROC_SUSPENDED	= 1,
	RPROC_RUNNING	= 2,
	RPROC_CRASHED	= 3,
	RPROC_DELETED	= 4,
	RPROC_ATTACHED	= 5,
	RPROC_DETACHED	= 6,
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
	RPROC_RUNNING_INDEPENDENT = 7,
	RPROC_LAST	= 8,
#else
	RPROC_LAST	= 7,
#endif
};

/**
 * enum rproc_crash_type - remote processor crash types
 * @RPROC_MMUFAULT:	iommu fault
 * @RPROC_WATCHDOG:	watchdog bite
 * @RPROC_FATAL_ERROR:	fatal error
 *
 * Each element of the enum is used as an array index. So that, the value of
 * the elements should be always something sane.
 *
 * Feel free to add more types when needed.
 */
enum rproc_crash_type {
	RPROC_MMUFAULT,
	RPROC_WATCHDOG,
	RPROC_FATAL_ERROR,
};

/**
 * enum rproc_dump_mechanism - Coredump options for core
 * @RPROC_COREDUMP_DISABLED:	Don't perform any dump
 * @RPROC_COREDUMP_ENABLED:	Copy dump to separate buffer and carry on with
 *				recovery
 * @RPROC_COREDUMP_INLINE:	Read segments directly from device memory. Stall
 *				recovery until all segments are read
 */
enum rproc_dump_mechanism {
	RPROC_COREDUMP_DISABLED,
	RPROC_COREDUMP_ENABLED,
	RPROC_COREDUMP_INLINE,
};

/**
 * struct rproc_dump_segment - segment info from ELF header
 * @node:	list node related to the rproc segment list
 * @da:		device address of the segment
 * @size:	size of the segment
 * @priv:	private data associated with the dump_segment
 * @dump:	custom dump function to fill device memory segment associated
 *		with coredump
 * @offset:	offset of the segment
 */
struct rproc_dump_segment {
	struct list_head node;

	dma_addr_t da;
	size_t size;

	void *priv;
	void (*dump)(struct rproc *rproc, struct rproc_dump_segment *segment,
		     void *dest, size_t offset, size_t size);
	loff_t offset;
};

#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
struct rproc_version {
	char version[VERSION_LEN];
	char compile_time[COMPILE_TIME_LEN];
	char git_hash_id[GIT_HASH_ID_LEN];
};
#endif

#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
struct rproc_mcu_version {
	char version[MCU_VERSION_LEN];
	char build_time[MCU_VERSION_LEN];
	char commoit_info[MCU_VERSION_LEN];
};
#endif
/**
 * enum rproc_features - features supported
 *
 * @RPROC_FEAT_ATTACH_ON_RECOVERY: The remote processor does not need help
 *				   from Linux to recover, such as firmware
 *				   loading. Linux just needs to attach after
 *				   recovery.
 */

enum rproc_features {
	RPROC_FEAT_ATTACH_ON_RECOVERY,
	RPROC_MAX_FEATURES,
};

/**
 * struct rproc - represents a physical remote processor device
 * @node: list node of this rproc object
 * @domain: iommu domain
 * @name: human readable name of the rproc
 * @firmware: name of firmware file to be loaded
 * @priv: private data which belongs to the platform-specific rproc module
 * @ops: platform-specific start/stop rproc handlers
 * @dev: virtual device for refcounting and common remoteproc behavior
 * @power: refcount of users who need this rproc powered up
 * @state: state of the device
 * @dump_conf: Currently selected coredump configuration
 * @lock: lock which protects concurrent manipulations of the rproc
 * @dbg_dir: debugfs directory of this rproc device
 * @traces: list of trace buffers
 * @num_traces: number of trace buffers
 * @carveouts: list of physically contiguous memory allocations
 * @mappings: list of iommu mappings we initiated, needed on shutdown
 * @bootaddr: address of first instruction to boot rproc with (optional)
 * @rvdevs: list of remote virtio devices
 * @subdevs: list of subdevices, to following the running state
 * @notifyids: idr for dynamically assigning rproc-wide unique notify ids
 * @index: index of this rproc device
 * @crash_handler: workqueue for handling a crash
 * @crash_cnt: crash counter
 * @recovery_disabled: flag that state if recovery was disabled
 * @max_notifyid: largest allocated notify id.
 * @table_ptr: pointer to the resource table in effect
 * @clean_table: copy of the resource table without modifications.  Used
 *		 when a remote processor is attached or detached from the core
 * @cached_table: copy of the resource table
 * @table_sz: size of @cached_table
 * @has_iommu: flag to indicate if remote processor is behind an MMU
 * @auto_boot: flag to indicate if remote processor should be auto-started
 * @sysfs_read_only: flag to make remoteproc sysfs files read only
 * @dump_segments: list of segments in the firmware
 * @nb_vdev: number of vdev currently handled by rproc
 * @elf_class: firmware ELF class
 * @elf_machine: firmware ELF machine
 * @cdev: character device of the rproc
 * @cdev_put_on_release: flag to indicate if remoteproc should be shutdown on @char_dev release
 * @features: indicate remoteproc features
 */
struct rproc {
	struct list_head node;
	struct iommu_domain *domain;
	const char *name;
	const char *firmware;
	void *priv;
	struct rproc_ops *ops;
	struct device dev;
	atomic_t power;
	unsigned int state;
	enum rproc_dump_mechanism dump_conf;
	struct mutex lock;
	struct dentry *dbg_dir;
	struct list_head traces;
	int num_traces;
	struct list_head carveouts;
	struct list_head mappings;
	u64 bootaddr;
	struct list_head rvdevs;
	struct list_head subdevs;
	struct idr notifyids;
	int index;
	struct work_struct crash_handler;
	unsigned int crash_cnt;
	bool recovery_disabled;
	int max_notifyid;
	struct resource_table *table_ptr;
	struct resource_table *clean_table;
	struct resource_table *cached_table;
	size_t table_sz;
	bool has_iommu;
	bool auto_boot;
	bool sysfs_read_only;
	struct list_head dump_segments;
	int nb_vdev;
	u8 elf_class;
	u16 elf_machine;
	struct cdev cdev;
	bool cdev_put_on_release;
	DECLARE_BITMAP(features, RPROC_MAX_FEATURES);
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	unsigned int fix_map_mode;
	struct rproc_version version;
#endif
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
	bool autonomous;
	unsigned int register_virtio;
	struct rproc_mcu_version sbl_version;
	struct rproc_mcu_version mcu_version;
#endif
};

/**
 * struct rproc_subdev - subdevice tied to a remoteproc
 * @node: list node related to the rproc subdevs list
 * @prepare: prepare function, called before the rproc is started
 * @start: start function, called after the rproc has been started
 * @stop: stop function, called before the rproc is stopped; the @crashed
 *	    parameter indicates if this originates from a recovery
 * @unprepare: unprepare function, called after the rproc has been stopped
 */
struct rproc_subdev {
	struct list_head node;

	int (*prepare)(struct rproc_subdev *subdev);
	int (*start)(struct rproc_subdev *subdev);
	void (*stop)(struct rproc_subdev *subdev, bool crashed);
	void (*unprepare)(struct rproc_subdev *subdev);
};

/* we currently support only two vrings per rvdev */

#define RVDEV_NUM_VRINGS 2

/**
 * struct rproc_vring - remoteproc vring state
 * @va:	virtual address
 * @num: vring size
 * @da: device address
 * @align: vring alignment
 * @notifyid: rproc-specific unique vring index
 * @rvdev: remote vdev
 * @vq: the virtqueue of this vring
 */
struct rproc_vring {
	void *va;
	int num;
	u32 da;
	u32 align;
	int notifyid;
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	int vring_size;
#endif
	struct rproc_vdev *rvdev;
	struct virtqueue *vq;
};

/**
 * struct rproc_vdev - remoteproc state for a supported virtio device
 * @subdev: handle for registering the vdev as a rproc subdevice
 * @pdev: remoteproc virtio platform device
 * @id: virtio device id (as in virtio_ids.h)
 * @node: list node
 * @rproc: the rproc handle
 * @vring: the vrings for this vdev
 * @rsc_offset: offset of the vdev's resource entry
 * @index: vdev position versus other vdev declared in resource table
 */
struct rproc_vdev {

	struct rproc_subdev subdev;
	struct platform_device *pdev;

	unsigned int id;
	struct list_head node;
	struct rproc *rproc;
	struct rproc_vring vring[RVDEV_NUM_VRINGS];
	u32 rsc_offset;
	u32 index;
#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
	struct completion config_wait_complete;
	int buffer_size;
	ulong buffer_phy_addr;
#endif
};

#if defined(CONFIG_HOBOT_REMOTEPROC) || defined(CONFIG_HOBOT_REMOTEPROC_MODULE)
struct fw_rsc_devmem_with_type {
	uint32_t typefw_rsc_devmem;
	uint32_t da;
	uint32_t pa;
	uint32_t len;
	uint32_t flags;
	uint32_t reserved;
	uint8_t name[32];
};

struct fw_rsc_version_with_type {
	uint32_t typefw_rsc_version;
	uint8_t version[48];
	uint8_t compile_time[16];
	uint8_t git_hash_id[40];
};

struct fw_rsc_vdev_with_type {
	uint32_t typefw_rsc_vdev;
	uint32_t id;
	uint32_t notifyid;
	uint32_t dfeatures;
	uint32_t gfeatures;
	uint32_t buffer_size;
	uint32_t config_len;
	uint8_t status;
	uint8_t num_of_vrings;
	uint8_t reserved[2];
	struct fw_rsc_vdev_vring vring[0]; /*PRQA S ALL*/
};

struct fw_rsc_rproc_mem_with_type{
	uint32_t typefw_rsc_rproc_mem;
	uint32_t da;
	uint32_t pa;
	uint32_t len;
	uint32_t reserved;
};

struct fw_rsc_fw_chksum_with_type {
	uint32_t typefw_rsc_rproc_mem;
	uint8_t algo[16];
	uint8_t chksum[64];
};

#define NO_RESOURCE_ENTRIES         8u

/* Resource table for the given remote */
struct remote_resource_table {
	uint32_t version;
	uint32_t num;
	uint32_t reserved[2];
	uint32_t offset[NO_RESOURCE_ENTRIES];
	struct fw_rsc_version_with_type fw_version; /*PRQA S ALL*/
	/* rproc memory entry */
	struct fw_rsc_rproc_mem_with_type rproc_mem;
	/* firmware checksum */
	struct fw_rsc_fw_chksum_with_type fw_chksum;
	/* itcm devmem entry */
	struct fw_rsc_devmem_with_type itcm_devmem;
	/* dtcm devmem entry */
	struct fw_rsc_devmem_with_type dtcm_devmem;
	/* rpmsg vdev entry */
	struct fw_rsc_vdev_with_type rpmsg_vdev; /*PRQA S ALL*/
	struct fw_rsc_vdev_vring rpmsg_vring0;
	struct fw_rsc_vdev_vring rpmsg_vring1;
}__attribute__((packed, aligned(0x100)));
#endif
struct rproc *rproc_get_by_phandle(phandle phandle);
struct rproc *rproc_get_by_child(struct device *dev);

struct rproc *rproc_alloc(struct device *dev, const char *name,
			  const struct rproc_ops *ops,
			  const char *firmware, int len);
void rproc_put(struct rproc *rproc);
int rproc_add(struct rproc *rproc);
int rproc_del(struct rproc *rproc);
void rproc_free(struct rproc *rproc);
void rproc_resource_cleanup(struct rproc *rproc);

struct rproc *devm_rproc_alloc(struct device *dev, const char *name,
			       const struct rproc_ops *ops,
			       const char *firmware, int len);
int devm_rproc_add(struct device *dev, struct rproc *rproc);

void rproc_add_carveout(struct rproc *rproc, struct rproc_mem_entry *mem);

struct rproc_mem_entry *
rproc_mem_entry_init(struct device *dev,
		     void *va, dma_addr_t dma, size_t len, u32 da,
		     int (*alloc)(struct rproc *, struct rproc_mem_entry *),
		     int (*release)(struct rproc *, struct rproc_mem_entry *),
		     const char *name, ...);

struct rproc_mem_entry *
rproc_of_resm_mem_entry_init(struct device *dev, u32 of_resm_idx, size_t len,
			     u32 da, const char *name, ...);

int rproc_boot(struct rproc *rproc);
int rproc_shutdown(struct rproc *rproc);
int rproc_detach(struct rproc *rproc);
int rproc_set_firmware(struct rproc *rproc, const char *fw_name);
#if defined(CONFIG_HOBOT_MCU_REMOTEPROC)
int rproc_boot_mcu(struct rproc *rproc);
int rproc_boot_testcase(struct rproc *rproc);
void rproc_mcu_reset(struct rproc *rproc);
void rproc_shutdown_mcu(struct rproc *rproc);
#endif
void rproc_report_crash(struct rproc *rproc, enum rproc_crash_type type);
void *rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem);
void rproc_coredump_using_sections(struct rproc *rproc);
int rproc_coredump_add_segment(struct rproc *rproc, dma_addr_t da, size_t size);
int rproc_coredump_add_custom_segment(struct rproc *rproc,
				      dma_addr_t da, size_t size,
				      void (*dumpfn)(struct rproc *rproc,
						     struct rproc_dump_segment *segment,
						     void *dest, size_t offset,
						     size_t size),
				      void *priv);
int rproc_coredump_set_elf_info(struct rproc *rproc, u8 class, u16 machine);

void rproc_add_subdev(struct rproc *rproc, struct rproc_subdev *subdev);

void rproc_remove_subdev(struct rproc *rproc, struct rproc_subdev *subdev);

#endif /* REMOTEPROC_H */
