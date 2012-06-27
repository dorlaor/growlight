#ifndef GROWLIGHT_GROWLIGHT
#define GROWLIGHT_GROWLIGHT

#ifdef __cplusplus
extern "C" {
#endif

int verbf(const char *,...) __attribute__ ((format (printf,1,2)));

#include <wchar.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>

#include "mounts.h"

#define FSLABELSIZ 17

	// This isn't really suitable for use as a library to programs beyond
	// growlight. Not yet, in any case.

struct controller;

// Growlight's callback-based UI
typedef struct growlight_ui {
	void (*vdiag)(const char *,va_list); // free-form diagnostics
	void *(*adapter_event)(const struct controller *,void *);
} glightui;

int growlight_init(int,char * const *,const glightui *);
int growlight_stop(void);

struct device;

typedef struct mdslave {
	char *name;			// Name of component
	struct device *component;	// Block device holding component of
					//  mdadm device
	struct mdslave *next;		// Next in this md device
} mdslave;

typedef enum {
	TRANSPORT_UNKNOWN,
	PARALLEL_ATA,
	SERIAL_UNKNOWN,
	SERIAL_ATA8,
	SERIAL_ATAI,
	SERIAL_ATAII,
	SERIAL_ATAIII,
	SERIAL_USB,
	SERIAL_USB2,
	SERIAL_USB3,
	AGGREGATE_UNKNOWN,
	AGGREGATE_MIXED,
} transport_e;

// An (non-link) entry in the device hierarchy, representing a block device.
// A partition corresponds to one and only one block device (which of course
// might represent multiple devices, or maybe just a file mounted loopback).
typedef struct device {
	struct device *next;		// next block device on this controller
	char name[NAME_MAX];		// Entry in /dev or /sys/block
	char *model,*revision,*sn;	// Arbitrary UTF-8 strings
	char *wwn;			// World Wide Name

	// Filesystem information. Block devices can have a (single) filesystem
	// if they don't have a partition table. The filesystem need not match
	// the container size, though it is generally unsafe to actually mount
	// a filesystem which is larger than its container.
	char *mnt;			// Active mount point
	char *mntops;			// Mount options
	uintmax_t mntsize;		// Filesystem size in bytes
	// If the filesystem is not mounted, but is found, only mnttype will be
	// set from among mnt, mntops and mnttype
	char *mnttype;			// Type of mount
	mntentry *target;		// Future mount point
	uintmax_t size;			// Size in bytes of device
	unsigned logsec;		// Logical sector size in bytes
	unsigned physsec;		// Physical sector size in bytes
	// Ranges from 0 to 32565, 0 highest priority. For our purposes, we
	// also use -1, indicating "unused", and -2, indicating "not swap".
	enum {
		SWAP_INVALID = -2,
		SWAP_INACTIVE = -1,
		SWAP_MAXPRIO = 0,
		SWAP_MINPRIO = 65535,
	} swapprio;			// Priority as a swap device
	char *uuid;			// *Filesystem* UUID
	char *label;			// *Filesystem* label
	union {
		struct {
			transport_e transport;
			unsigned realdev: 1;	// Is itself a real block device
			unsigned removable: 1;	// Removable media
			unsigned rotate: 1;	// Rotational media / spinning platters
			unsigned wcache: 1;	// Write cache enabled
			unsigned biosboot: 1;	// Non-zero bytes in MBR code area
			unsigned smart: 1;	// SMART support
			void *biossha1;		// SHA1 of first 440 bytes
			char *pttable;		// Partition table type (can be NULL)
			char *serial;		// Serial number (can be NULL)
		} blkdev;
		struct {
			unsigned long disks;	// RAID disks in md
			char *level;		// RAID level
			mdslave *slaves;	// RAID components
			char *uuid;
			char *mdname;
			transport_e transport;
		} mddev;
		struct {
			// The *partition* UUID, not the filesystem/disk's.
			char *uuid;
			wchar_t *pname;		// Partition name, if it has
						//  one (GPT has a UTF-16 name).
			unsigned pnumber;	// Partition number
			// The BIOS+MBR partition record (including the first
			// byte, the 'boot flag') and GPT attributes.
			unsigned long long flags;
			enum {
				PARTROLE_UNKNOWN,
				PARTROLE_PRIMARY,	// BIOS+MBR
				PARTROLE_EXTENDED,
				PARTROLE_LOGICAL,
				PARTROLE_EPS,		// UEFI
				PARTROLE_GPT,
				PARTROLE_MAC,
				PARTROLE_PC98,
			} partrole;
			struct device *parent;
		} partdev;
		struct {
			transport_e transport;
			uint64_t zpoolver;	// zpool version
			unsigned long disks;	// vdevs in zpool
			char *level;		// zraid level
			unsigned state;		// POOL_STATE_[UN]AVAILABLE
		} zpool;
	};
	enum {
		LAYOUT_NONE,
		LAYOUT_MDADM,
		LAYOUT_PARTITION,
		LAYOUT_ZPOOL,
	} layout;
	struct device *parts;	// Partitions (can be NULL)
	dev_t devno;		// Don't expose this non-persistent datum
} device;

// A block device controller.
typedef struct controller {
	// FIXME if libpci doesn't know about the device, we still ought use
	// a name determined via inspection of sysfs, just as we do for disks
	char *name;		// From libpci database
	char *sysfs;		// Sysfs node
	char *driver;		// From sysfs, 'device/module'
	char *ident;		// Manufactured identifier to reference adapter
	enum {
		BUS_UNKNOWN,
		BUS_VIRTUAL,
		BUS_PCIe,
	} bus;
	enum {
		TRANSPORT_ATA,
		TRANSPORT_USB,
		TRANSPORT_USB2,
		TRANSPORT_USB3,
	} transport;
	// Union parameterized on bus type
	union {
		struct {
			// PCIe theoretical speeds reach (1 transfer == 1 bit):
			//
			//  1.0: 2.5GT/s each way
			//  2.0: 5GT/s each way
			//  3.0: 8GT/s each way
			//
			// 1.0 and 2.0 use 8/10 encoding, while 3.0 uses 8/128
			// encoding. 1.0 thus gives you a peak of 250MB/s/lane,
			// and 2.0 offers 500MB/s/lane. Further overheads can
			// reduce the useful throughput.
			unsigned gen;
			// A physical slot can be incompletely wired, allowing
			// a card of n lanes to be used in a slot with only m
			// electronically-wired lanes, n > m.
			//
			//  lanes_cap: card capabilities
			//  lanes_sta: negotiated number of PCIe lanes
			unsigned lanes_cap,lanes_neg;
			// PCIe topological addressing
			unsigned domain,bus,dev,func;
		} pcie;
	};
	device *blockdevs;
	struct controller *next;
	dev_t devno;		// Don't expose this non-persistent datum
} controller;

// Currently, we just blindly hand out references to our internal store. This
// simply will not fly in the long run -- FIXME
const controller *get_controllers(void);

// These are similarly no good FIXME
device *lookup_device(const char *name);
controller *lookup_controller(const char *name);

// Supported partition table types
const char **get_ptable_types(void);

// Supported filesystem types
const char **get_fs_types(void);

int make_partition_table(device *,const char *);

int reset_controller(controller *);
int rescan_controller(controller *);

int rescan_blockdev(device *);
int benchmark_blockdev(const device *);
void free_device(device *);

// Very coarse locking
int lock_growlight(void);
int unlock_growlight(void);
int rescan_device(const char *);
int rescan_devices(void);

void add_new_virtual_blockdev(device *);

int prepare_bios_boot(device *);
int prepare_uefi_boot(device *);

#ifdef __cplusplus
}
#endif

#endif
