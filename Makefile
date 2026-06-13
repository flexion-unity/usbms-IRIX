#!smake
#
# Makefile for the IRIX 6.5 USB Mass-Storage loadable kernel driver "usbms".
# Use SGI smake (the #!smake line makes `make` re-exec under smake).
#
# Build:   make
# Load:    make load       (ml ld -v -b ./usbms.o -p usbms_)
# Unload:  make unload
# Install: make install    (copies into /var/sysgen; then autoconfig && reboot)
# Remove:  make uninstall  (removes from /var/sysgen; then autoconfig && reboot)
#
# Requires the `usb` framework module to be loadable (it ships in
# /var/sysgen/boot/usb.a) so the usb_* symbols resolve at ml-load time.

ROOT=/
include $(ROOT)/usr/include/make/commondefs
CPUBOARD != uname -m

# ---- flags shared by all IRIX loadable drivers (see fl example for meanings) ----
CFLAGS= -D_KERNEL -DMP_STREAMS -D_MP_NETLOCKS -DMRSP_AS_MR \
	-fullwarn -non_shared -G 0 -TARG:force_jalr \
	-TENV:kernel -OPT:space -OPT:Olimit=0 -CG:unique_exit=on \
	-TENV:X=1 -OPT:IEEE_arithmetic=1 -OPT:roundoff=0 \
	-OPT:wrap_around_unsafe_opt=off

# ---- 64-bit ----
CFLAGS+= -64 -D_PAGESZ=16384

# ---- IP35 (Fuel / Tezro / Origin 3x00) ----
#if $(CPUBOARD) == "IP35"
CFLAGS+= -DIP35 -DR10000 -DMP -DSN1 -DSN \
	-DMAPPED_KERNEL -DLARGE_CPU_COUNT -DPTE_64BIT -DULI -DCKPT -DMIPS4_ISA \
	-DNUMA_BASE -DNUMA_PM -DNUMA_TBORROW -DNUMA_MIGR_CONTROL \
	-DNUMA_REPLICATION -DNUMA_REPL_CONTROL -DNUMA_SCHED -DCELL_PREPARE \
	-DBHV_PREPARE -TARG:processor=r10000
#endif

all: usbms.o

usbms.o: usbms.c usbdi.h
	-if [ "${CCOPTS}" = "" ]; then \
		cc $(CFLAGS) -c usbms.c; \
	else \
		cc $(CFLAGS) $(CCOPTS) -c usbms.c; \
	fi

# preprocess only (debugging compile errors)
cpp: usbms.c usbdi.h
	cc -E $(CFLAGS) usbms.c > usbms.cpp

clean:
	rm -f usbms.o usbms.cpp

load: usbms.o
	-@U=`ml list | grep usbms_ | awk '{print $$2}'`; \
	if [ -n "$$U" ]; then echo "ml unld -v $$U"; ml unld -v $$U; fi; \
	echo "ml ld -v -b ./usbms.o -p usbms_"; \
	ml ld -v -b ./usbms.o -p usbms_

unload:
	-@U=`ml list | grep usbms_ | awk '{print $$2}'`; \
	if [ -n "$$U" ]; then echo "ml unld -v $$U"; ml unld -v $$U; fi

install: usbms.o
	cp usbms.o      /var/sysgen/boot/usbms.o
	cp usbms.master /var/sysgen/master.d/usbms
	cp usbms.sm     /var/sysgen/system/usbms.sm
	@echo "Installed. Now run: autoconfig -v && reboot   (or 'make reg' to autoload without a reboot)"

uninstall:
	rm -f /var/sysgen/boot/usbms.o
	rm -f /var/sysgen/master.d/usbms
	rm -f /var/sysgen/system/usbms.sm
	@echo "Uninstalled. Now run: autoconfig -v && reboot   to rebuild the kernel without usbms"

reg: usbms.o
	ml reg -v -b ./usbms.o -p usbms_

unreg:
	-@U=`ml list -r | grep usbms_ | awk '{print $$2}'`; \
	if [ -n "$$U" ]; then ml unreg -v $$U; fi
