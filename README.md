# usbms-IRIX
USB mass-storage driver for SiliconGraphics IRIX (IP35)

A loadable kernel **class driver** that plugs into IRIX's existing in-kernel USB
stack (`/var/sysgen/boot/usb.a` = OHCI HCD + USB core + hub) and presents a USB
mass-storage device as an IRIX block/character device.

This is an experimental driver. Use at your own risk!


## Prerequisites

- IP35 hardware (Tezro/Fuel/O350)
- IRIX 6.5.x
- MIPSPro C compiler 7.4.4

## Build the driver

```sh
make
```

## Installation

### Option 1: Loadable module

Load the module into the running kernel without rebooting:

```sh
# Load into kernel
make load
```

To unload the module:

```sh
make unload
```

### Option 2: Permanent installation

Install the module as a permanent part of the kernel, loaded when booting.


```sh
# Install files to system directories
make install
autoconfig -v

# Or to uninstall
make uninstall
```

This copies:
- `usbms.o` → `/var/sysgen/boot/` (kernel module)
- `usbms.master` → `/var/sysgen/master.d/usbms` (master configuration)
- `usbms.sm` → `/var/sysgen/system/` (system configuration)


## Mount media

### XFS

When using XFS, make sure to use the older XFS format for IRIX. 

To format on Linux using older XFS format:

```sh
mkfs.xfs -m crc=0 -n ftype=0 -i projid32bit=0 -f /dev/sdX
```

Mount in IRIX:

```sh
mount -t xfs /hw/usb/disk/0/block /YOUR/MOUNTPOINT
```


### FAT32

To read fat32 partitions, you first need to install the [fat32 module](https://github.com/techomancer/fat32).

Mounting FAT32 does not work yet and is under investigation.


