/*
 * usbdi.h — IRIX 6.5.30 USBDI (USB Driver Interface) declarations.
 *
 * SGI shipped no USB headers; these prototypes were RECOVERED BY DISASSEMBLY
 * of /var/sysgen/boot/usb.a + usb_pckm.o + usbaudio_dd.o.  See
 * ../irix-recon/usbdi_recovered.h for the evidence and per-symbol confidence.
 *
 * The symbols are exported by the in-kernel `usb` module; usbms declares a
 * DEPENDENCY on `usb` in its master.d so ml(1M) resolves them at load time.
 *
 * Calling convention: MIPS n64.  All handles are opaque pointers.
 */
#ifndef _USBMS_USBDI_H_
#define _USBMS_USBDI_H_

/* ---- usb_status_t: tagged 0x625f0000 ('b''_'<<16) | errno-ish low byte ---- */
typedef int usb_status_t;
#define USB_OK        0x625f0000
#define USB_EINVAL    0x625f0005
#define USB_ENOMEM    0x625f000e
#define USB_EIO       0x625f0017
#define USB_ENOMATCH  0x625f0019
#define USB_IS_OK(s)  ((s) == USB_OK)

/* ---- opaque framework handles ---- */
typedef void *usb_iface_t;
typedef void *usb_pipe_t;
typedef void *usb_device_t;

/* ---- event delivered to a registered driver's handler ----
 * ev->type @ +0x00, ev->iface @ +0x08 (recovered from usb_audio_event_handler) */
struct usb_event {
	int          type;	/* +0x00 : USB_EVENT_*			*/
	int          _pad0;	/* +0x04					*/
	usb_iface_t  iface;	/* +0x08					*/
};
#define USB_EVENT_ATTACH   0
#define USB_EVENT_DETACH   1
#define USB_EVENT_SUSPEND  2
#define USB_EVENT_PROBE    3

typedef usb_status_t (*usb_event_handler_t)(struct usb_event *ev);

/* ---- registration ---- */
extern usb_status_t usb_driver_register(usb_event_handler_t, const char *name, void *priv);
extern usb_status_t usb_driver_unregister(usb_event_handler_t, const char *name, void *priv);

/* ---- interface introspection ---- */
extern int          usb_iface_class(usb_iface_t);
extern int          usb_iface_subclass(usb_iface_t);
extern void        *usb_iface_desc(usb_iface_t);
extern int          usb_iface_desclen(usb_iface_t);
extern int          usb_iface_npipe(usb_iface_t);
extern usb_pipe_t   usb_iface_defpipe(usb_iface_t);
extern usb_device_t usb_iface_device(usb_iface_t);
extern int          usb_iface_number(usb_iface_t);
extern int          usb_iface_port(usb_iface_t);

/* ---- descriptor walking: index-th descriptor of `type` in buf[0..len] ---- */
extern void *usb_descriptor_lookup(void *buf, int buflen, int desc_type, int index);
#define USB_DESC_DEVICE     0x01
#define USB_DESC_CONFIG     0x02
#define USB_DESC_INTERFACE  0x04
#define USB_DESC_ENDPOINT   0x05

/* ---- pipes ---- */
/* usb_pipe_open(iface, sel, ep_desc, &out): sel = -1 means "match the endpoint
 * described by ep_desc" (recovered from usb_hid_attach; sel=0 returns the wrong
 * pipe).  a0=iface, a3=output handle. */
extern usb_status_t usb_pipe_open(usb_iface_t iface, int sel, void *ep_desc, usb_pipe_t *out);
extern void         usb_pipe_close(usb_pipe_t);
extern usb_status_t usb_pipe_clear_halt(usb_pipe_t);
extern int          usb_pipe_maxpacket(usb_pipe_t);

/* ---- transfers ----
 * synch: builds a transfer block, drives usb_ohci_transfer_start, blocks on a
 *   sync var until the OHCI completion interrupt (or timeout) fires.  Recovered:
 *     a0 pipe, a1 buf, a2 len,
 *     a3 = int* actual-length out (= len - residue); NULL-CHECKED, so NULL ok,
 *     a4 = flags (-> tb[92]; 0 is safe),
 *     a5 = timeout in MILLISECONDS (must be > 0; internally *1000 -> usectohz).
 *   Returns tb[72] (final status).  timeout status == 0x625f000b. */
extern usb_status_t usb_bulk_trans_synch(usb_pipe_t pipe, void *buf, int len,
                                         int *actlen_out, int flags, int timeout_ms);
extern usb_status_t usb_bulk_trans(usb_pipe_t, void *buf, int len, void *cb, void *cb_arg, int flags, int tmo);

/* ---- control transfers ---- */
/*
 * usb_setup_init() builds the 8-byte SETUP packet.  The bmRequestType byte is
 * passed EXPANDED into its three sub-fields, and wLength comes LAST - eight
 * args total (verified by disassembly of the IRIX usb.o):
 *     setup8[0] = (dir&1)<<7 | (type&3)<<5 | (recip&0x1f)   bmRequestType
 *     setup8[1] = bRequest
 *     setup8[2:3] = wValue   (LE)
 *     setup8[4:5] = wIndex   (LE)
 *     setup8[6:7] = wLength  (LE)   <- usb_command() copies this many data bytes
 *   dir:   0 = OUT (host->dev), 1 = IN (dev->host)
 *   type:  0 = standard, 1 = class, 2 = vendor
 *   recip: 0 = device, 1 = interface, 2 = endpoint
 * NOTE: a previous 6-arg declaration (packed bmRequestType, wLength 6th) left
 * wLength sourced from an unset arg register; usb_command() then memcpy'd that
 * garbage byte-count into the caller's data buffer and panicked.
 */
extern void         usb_setup_init(void *setup8, int dir, int type, int recip,
                                   int bReq, int wValue, int wIndex, int wLength);
extern usb_status_t usb_command(usb_pipe_t ctrl_pipe, void *setup8, void *data, int length);

/* ---- IRIX glue (usb_irix.o) ----
 * RECOVERED usb_malloc(size, a1, zero_select, kmflags):
 *   zero_select != 0 -> kmem_zalloc(size, kmflags)  (zeroed)
 *   zero_select == 0 -> kmem_alloc(size, kmflags)
 * kmflags is the IRIX kmem flag (0 = KM_SLEEP).  The framework calls
 * usb_malloc(size, 0, 1, 0).  NB: the 4th arg is REQUIRED — omitting it leaves
 * garbage kmem flags and panics in page.c. */
extern void *usb_malloc(unsigned long size, int a1, int zero_select, int kmflags);
extern void  usb_free(void *);
extern void  usb_usdelay(int usec);

/* DMA mapping (usb_irix.o): usb_dmamap(hc, buf, len, flags, &cookie) does
 *   kvtophys(buf) then pciio_dma_addr(hc->[44], ...) and writes the 32-bit PCI
 *   BUS address to cookie[16], length to cookie[20], unmap handle to cookie[8].
 *   `hc` is the OHCI host-controller context (reachable at pipe[32][64], the way
 *   usb_ohci_command_start derives it).  flags=1 (used by the control path for
 *   both directions).  usb_dmaunmap(&cookie) tears it down (+ cache coherency).
 * Needed because usb_ohci_schedule_bulk — unlike the control path — never maps
 * the buffer, so we must hand it a bus address ourselves. */
extern void usb_dmamap(void *hc, void *buf, int len, int flags, void *cookie);
extern void usb_dmaunmap(void *cookie);

#endif /* _USBMS_USBDI_H_ */
