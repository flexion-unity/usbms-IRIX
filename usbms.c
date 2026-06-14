/*
 * usbms.c - USB Mass-Storage class driver for IRIX 6.5.30
 * SGI IP35 (Fuel/Tezro/O350), OHCI USB host controller
 *
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/buf.h>
#include <sys/uio.h>
#include <sys/open.h>
#include <sys/cred.h>
#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/ddi.h>
#include <sys/ksynch.h>
#include <sys/hwgraph.h>
#include <sys/invent.h>
#include <sys/mload.h>
#include <sys/sysmacros.h>

#include "usbdi.h"

int   usbms_devflag  = D_MP;		/* multiprocessor-safe		*/
char *usbms_mversion = M_VERSION;	/* must match running kernel	*/

#define USBMS_PREFIX  "usbms_"
int usbms_verbose = 0;
#define DPRINT(args)  do { if (usbms_verbose) cmn_err args; } while (0)
#define TRACE(msg)  DPRINT((CE_NOTE, "usbms_setup: " msg "\n"))

#define MSC_CLASS	0x08
#define MSC_SUBCLASS_SCSI	0x06	/* SCSI transparent		*/
#define MSC_PROTO_BBB	0x50		/* Bulk-Only Transport		*/

#define MSC_REQ_RESET	0xFF		/* bmRequestType 0x21		*/
#define MSC_REQ_GET_MAX_LUN	0xFE	/* bmRequestType 0xA1		*/

#define ED_bLength		0
#define ED_bDescriptorType	1
#define ED_bEndpointAddress	2	/* bit7: 1=IN 0=OUT		*/
#define ED_bmAttributes		3	/* low 2 bits: 0=ctrl 2=bulk	*/
#define EP_DIR_IN		0x80
#define EP_XFER_BULK		0x02

#define CBW_LEN		31
#define CSW_LEN		13
#define CBW_SIGNATURE	0x43425355U	/* 'USBC' */
#define CSW_SIGNATURE	0x53425355U	/* 'USBS' */
#define CBW_FLAG_IN	0x80
#define CSW_STATUS_PASS	0

#define USBMS_TIMEOUT	5000		/* ms per bulk transfer		*/

/*
 * Transfer flags (the 5th arg to usb_bulk_trans_synch -> tb[92]).  RECOVERED:
 * usb_ohci_schedule_bulk gates on (tb[92] & 0x4) - if that bit is CLEAR it
 * returns USB_EINVAL without building a TD, and start_wq_lock then spins
 * forever on the un-advanced queue head (hard freeze).  Bit 0x4 selects the
 * general-TD scheduling path that bulk requires; it MUST be set.
 */
#define USBMS_XFLAG	0x4
#define USBMS_DMA_MAX	(64*1024)	/* bounce buffer size (page-aligned)	*/
/*
 * Max bytes per BULK transfer.  Capped so each transfer is (a) within one
 * OHCI general TD (~8 KB max), and (b) within one physically-contiguous span
 * of the bounce buffer - usb_dmamap only maps a contiguous region, and IP35
 * pages are 16 KB, so an 8 KB transfer from the page-aligned buffer never
 * crosses a page boundary.  Larger transfers (e.g. XFS's 64 KB) are looped.
 */
#define USBMS_XFER_MAX	8192
#define USBMS_SECTOR	512		/* IRIX block (BBSIZE)		*/

/* little-endian (USB wire) accessors over a byte buffer */
#define LE32_PUT(p,o,v)	do { (p)[(o)+0]=(unsigned char)((v));        \
			     (p)[(o)+1]=(unsigned char)((v)>>8);    \
			     (p)[(o)+2]=(unsigned char)((v)>>16);   \
			     (p)[(o)+3]=(unsigned char)((v)>>24); } while (0)
#define LE32_GET(p,o)	((__uint32_t)(p)[(o)+0]        |                \
			 ((__uint32_t)(p)[(o)+1] << 8)  |              \
			 ((__uint32_t)(p)[(o)+2] << 16) |              \
			 ((__uint32_t)(p)[(o)+3] << 24))
/* big-endian (SCSI CDB / data) accessors */
#define BE32_PUT(p,o,v)	do { (p)[(o)+0]=(unsigned char)((v)>>24);       \
			     (p)[(o)+1]=(unsigned char)((v)>>16);   \
			     (p)[(o)+2]=(unsigned char)((v)>>8);    \
			     (p)[(o)+3]=(unsigned char)((v)); } while (0)
#define BE16_PUT(p,o,v)	do { (p)[(o)+0]=(unsigned char)((v)>>8);        \
			     (p)[(o)+1]=(unsigned char)((v)); } while (0)
#define BE32_GET(p,o)	(((__uint32_t)(p)[(o)+0] << 24) |              \
			 ((__uint32_t)(p)[(o)+1] << 16) |              \
			 ((__uint32_t)(p)[(o)+2] << 8)  |              \
			 ((__uint32_t)(p)[(o)+3]))

/* SCSI opcodes */
#define SCSI_TEST_UNIT_READY	0x00
#define SCSI_REQUEST_SENSE	0x03
#define SCSI_INQUIRY		0x12
#define SCSI_READ_CAPACITY10	0x25
#define SCSI_READ10		0x28
#define SCSI_WRITE10		0x2A

/* ------------------------------------------------------------------ *
 * Per-device software context.  Pointed to by each hwgraph vertex via
 * device_info_set(); fetched in the devsw entry points with
 * device_info_get(dev).
 * ------------------------------------------------------------------ */
struct usbms_softc {
	usb_iface_t	sc_iface;
	usb_pipe_t	sc_pipe_in;	/* bulk IN			*/
	usb_pipe_t	sc_pipe_out;	/* bulk OUT			*/
	void	       *sc_ep_in;	/* IN endpoint descriptor	*/
	void	       *sc_ep_out;	/* OUT endpoint descriptor	*/
	unsigned char	sc_addr_in;	/* bEndpointAddress (IN)	*/
	unsigned char	sc_addr_out;	/* bEndpointAddress (OUT)	*/
	unsigned char	sc_maxlun;	/* from GET MAX LUN		*/
	unsigned int	sc_blksize;	/* device sector size (READ CAP)	*/
	__uint64_t	sc_nblocks;	/* device sectors    (READ CAP)	*/
	int		sc_unit;
	int		sc_ready;	/* 0 until lazily set up in open()	*/
	int		sc_open;	/* open reference count		*/
	int		sc_gone;	/* device detached while open	*/
	__uint32_t	sc_tag;		/* BOT CBW tag sequence		*/
	unsigned char  *sc_cbw;		/* DMA: 31-byte CBW		*/
	unsigned char  *sc_csw;		/* DMA: 13-byte CSW		*/
	unsigned char  *sc_dma;		/* DMA: data bounce buffer	*/
	mutex_t	       *sc_lock;	/* serialises BOT commands	*/
	vertex_hdl_t	sc_vtx_base;
	vertex_hdl_t	sc_vtx_chr;
	vertex_hdl_t	sc_vtx_blk;
};

/* Attached instances, indexed by unit number. */
#define USBMS_MAXUNIT	4
static struct usbms_softc *usbms_units[USBMS_MAXUNIT];

/*
 * Interfaces we've accepted in probe.  The core probes an interface MULTIPLE
 * times before it attaches, so we must dedup at probe time (units[] isn't set
 * until attach) - otherwise the core logs a "driver conflict" on every re-probe.
 */
static usb_iface_t usbms_claimed[USBMS_MAXUNIT];

static int
usbms_is_claimed(usb_iface_t iface)
{
	int i;

	for (i = 0; i < USBMS_MAXUNIT; i++)
		if (usbms_claimed[i] == iface)
			return (1);
	return (0);
}

static void
usbms_claim(usb_iface_t iface)
{
	int i;

	for (i = 0; i < USBMS_MAXUNIT; i++)
		if (usbms_claimed[i] == NULL) {
			usbms_claimed[i] = iface;
			return;
		}
}

static void
usbms_unclaim(usb_iface_t iface)
{
	int i;

	for (i = 0; i < USBMS_MAXUNIT; i++)
		if (usbms_claimed[i] == iface)
			usbms_claimed[i] = NULL;
}

static struct usbms_softc *
usbms_find_by_iface(usb_iface_t iface)
{
	int i;

	for (i = 0; i < USBMS_MAXUNIT; i++)
		if (usbms_units[i] != NULL && usbms_units[i]->sc_iface == iface)
			return (usbms_units[i]);
	return (NULL);
}

/* ================================================================== *
 *  Probe / attach / detach
 * ================================================================== */

/*
 * Match an interface: Mass-Storage / SCSI-transparent / Bulk-Only.
 * class & subclass come from accessors; protocol is byte 7 of the raw
 * interface descriptor (no usb_iface_protocol export exists).
 */
static usb_status_t
usbms_probe(usb_iface_t iface)
{
	int cls  = usb_iface_class(iface);
	int sub  = usb_iface_subclass(iface);
	unsigned char *id = (unsigned char *)usb_iface_desc(iface);
	int proto = (id != NULL) ? id[7] : -1;	/* bInterfaceProtocol */

	/* Already accepted on an earlier probe of the same interface - don't
	 * accept again (avoids the core's "driver conflict" re-probe warning). */
	if (usbms_is_claimed(iface))
		return (USB_ENOMATCH);

	DPRINT((CE_NOTE, "usbms_probe: class=0x%x subclass=0x%x proto=0x%x\n",
	    cls, sub, proto));

	if (cls == MSC_CLASS && sub == MSC_SUBCLASS_SCSI && proto == MSC_PROTO_BBB) {
		usbms_claim(iface);
		return (USB_OK);		/* claim it */
	}

	return (USB_ENOMATCH);
}

/*
 * Walk the interface's endpoint descriptors and record the bulk IN/OUT
 * endpoints.  Returns 0 on success, errno on failure.
 */
static int
usbms_find_endpoints(struct usbms_softc *sc)
{
	void *desc = usb_iface_desc(sc->sc_iface);
	int   dlen = usb_iface_desclen(sc->sc_iface);
	int   i;

	sc->sc_ep_in = sc->sc_ep_out = NULL;

	for (i = 0; ; i++) {
		unsigned char *ep =
		    (unsigned char *)usb_descriptor_lookup(desc, dlen,
			USB_DESC_ENDPOINT, i);
		if (ep == NULL)
			break;
		if ((ep[ED_bmAttributes] & 0x03) != EP_XFER_BULK)
			continue;
		if (ep[ED_bEndpointAddress] & EP_DIR_IN) {
			sc->sc_ep_in   = ep;
			sc->sc_addr_in = ep[ED_bEndpointAddress];
		} else {
			sc->sc_ep_out   = ep;
			sc->sc_addr_out = ep[ED_bEndpointAddress];
		}
	}

	DPRINT((CE_NOTE, "usbms: endpoints bulk-IN=0x%x bulk-OUT=0x%x\n",
	    sc->sc_addr_in, sc->sc_addr_out));

	if (sc->sc_ep_in == NULL || sc->sc_ep_out == NULL)
		return (ENXIO);
	return (0);
}

/*
 * GET MAX LUN class request on the default control pipe.
 * First real exercise of usb_command(); failure is non-fatal (assume 0).
 */
static void
usbms_get_max_lun(struct usbms_softc *sc)
{
	/* Build the SETUP packet and receive the LUN byte in sc_dma (already
	 * allocated by this point in usbms_setup): the control path DMAs both, so
	 * keep them out of the kernel stack like every other transfer here. */
	unsigned char *setup = sc->sc_dma;	/* [0..7]  = setup packet */
	unsigned char *data  = sc->sc_dma + 8;	/* [8]     = returned LUN */
	usb_pipe_t ctrl = usb_iface_defpipe(sc->sc_iface);
	usb_status_t st;

	*data = 0;

	/* GET MAX LUN: bmRequestType 0xA1 = IN | class | interface.
	 * usb_setup_init() wants that byte EXPANDED into (dir,type,recip) and
	 * takes wLength LAST - see usbdi.h.  The old packed 6-arg form left
	 * wLength as a garbage arg register, which usb_command() used as its
	 * data-copy length and panicked (memcpy overrun of the data buffer). */
	usb_setup_init(setup,
	    1,					/* dir   = IN        */
	    1,					/* type  = class     */
	    1,					/* recip = interface */
	    MSC_REQ_GET_MAX_LUN,		/* bRequest          */
	    0,					/* wValue            */
	    usb_iface_number(sc->sc_iface),	/* wIndex            */
	    1);					/* wLength           */
	st = usb_command(ctrl, setup, data, 1);

	if (USB_IS_OK(st))
		sc->sc_maxlun = *data;
	else
		sc->sc_maxlun = 0;	/* STALL => single-LUN device */

	DPRINT((CE_NOTE, "usbms: GET MAX LUN st=0x%x maxlun=%d\n",
	    (unsigned)st, sc->sc_maxlun));
}

/* ================================================================== *
 *  Bulk-Only Transport (BOT) + minimal SCSI
 * ================================================================== */

/*
 * DMA-mapped bulk transfer.
 *
 * SGI's usb_ohci_schedule_bulk (unlike usb_ohci_command_start) NEVER DMA-maps
 * the transfer buffer - it writes the raw pointer straight into the OHCI TD's
 * CurrentBufferPointer, so on IP35 (IOMMU: phys != bus) the controller DMAs to
 * the wrong address and the machine freezes.  Work around the framework bug:
 * map the buffer ourselves with usb_dmamap (the HC context is at pipe[32][64],
 * exactly how command_start finds it) and pass usb_bulk_trans_synch the BUS
 * address; we still read/write the data through the kernel-virtual buffer.
 */
struct usbms_dma { unsigned char raw[32]; };	/* usb_dmamap cookie */

static usb_status_t
usbms_bulk(usb_pipe_t pipe, void *vbuf, int len, int *actlen)
{
	struct usbms_dma ck;
	unsigned char *pp  = (unsigned char *)pipe;
	unsigned char *p32 = *(unsigned char **)(pp + 32);
	void *hc = *(void **)(p32 + 64);		/* pipe[32][64] */
	unsigned int busaddr;
	usb_status_t st;

	bzero((void *)&ck, sizeof(ck));
	usb_dmamap(hc, vbuf, len, 1, &ck);
	busaddr = *(unsigned int *)(&ck.raw[16]);	/* cookie[16] = bus addr */
	if (busaddr == 0) {
		cmn_err(CE_WARN, "usbms_bulk: usb_dmamap(len=%d) failed\n", len);
		return (USB_EIO);
	}

	st = usb_bulk_trans_synch(pipe, (void *)(unsigned long)busaddr, len,
	    actlen, USBMS_XFLAG, USBMS_TIMEOUT);

	usb_dmaunmap((void *)&ck);
	return (st);
}

/*
 * One BOT command: CBW (OUT) -> optional DATA -> CSW (IN).
 * cdb/cdblen   : SCSI command block.
 * data/datalen : DMA bounce buffer (sc_dma) and byte count (0 if none).
 * dir_in       : 1 = device->host data, 0 = host->device.
 * Returns 0 on success (CSW status PASS), else EIO.
 *
 * NOTE: this is the first in-tree exercise of usb_bulk_trans_synch.  We treat
 * USB_OK as transfer success and log any other status so the real completion
 * code is revealed on first run.
 */
static int
usbms_bot(struct usbms_softc *sc, unsigned char *cdb, int cdblen,
    int datalen, int dir_in)
{
	unsigned char *cbw = sc->sc_cbw;
	unsigned char *csw = sc->sc_csw;
	__uint32_t tag = ++sc->sc_tag;
	usb_status_t st;
	int actlen, i;

	/* ---- CBW ---- */
	bzero(cbw, CBW_LEN);
	LE32_PUT(cbw, 0, CBW_SIGNATURE);
	LE32_PUT(cbw, 4, tag);
	LE32_PUT(cbw, 8, (__uint32_t)datalen);
	cbw[12] = dir_in ? CBW_FLAG_IN : 0;	/* bmCBWFlags */
	cbw[13] = 0;				/* bCBWLUN    */
	cbw[14] = (unsigned char)cdblen;	/* bCBWCBLength */
	for (i = 0; i < cdblen && i < 16; i++)
		cbw[15 + i] = cdb[i];

	st = usbms_bulk(sc->sc_pipe_out, cbw, CBW_LEN, NULL);
	if (!USB_IS_OK(st)) {
		cmn_err(CE_WARN, "usbms_bot: CBW(op=0x%x) st=0x%x\n",
		    cdb[0], (unsigned)st);
		usb_pipe_clear_halt(sc->sc_pipe_out);
		return (EIO);
	}

	/* ---- DATA (optional) ---- */
	if (datalen > 0) {
		usb_pipe_t dp = dir_in ? sc->sc_pipe_in : sc->sc_pipe_out;
		actlen = 0;
		st = usbms_bulk(dp, sc->sc_dma, datalen, &actlen);
		if (!USB_IS_OK(st)) {
			DPRINT((CE_NOTE, "usbms_bot: DATA st=0x%x (clearing halt)\n",
			    (unsigned)st));
			usb_pipe_clear_halt(dp);	/* proceed to CSW */
		}
	}

	/* ---- CSW ---- */
	bzero(csw, CSW_LEN);
	actlen = 0;
	st = usbms_bulk(sc->sc_pipe_in, csw, CSW_LEN, &actlen);
	if (!USB_IS_OK(st)) {
		usb_pipe_clear_halt(sc->sc_pipe_in);	/* one retry */
		st = usbms_bulk(sc->sc_pipe_in, csw, CSW_LEN, &actlen);
		if (!USB_IS_OK(st)) {
			cmn_err(CE_WARN, "usbms_bot: CSW(op=0x%x) st=0x%x\n",
			    cdb[0], (unsigned)st);
			return (EIO);
		}
	}

	if (LE32_GET(csw, 0) != CSW_SIGNATURE) {
		cmn_err(CE_WARN, "usbms_bot: bad CSW sig 0x%x\n",
		    (unsigned)LE32_GET(csw, 0));
		return (EIO);
	}
	if (LE32_GET(csw, 4) != tag) {
		cmn_err(CE_WARN, "usbms_bot: CSW tag mismatch (%u != %u)\n",
		    (unsigned)LE32_GET(csw, 4), (unsigned)tag);
		return (EIO);
	}
	if (csw[12] != CSW_STATUS_PASS) {
		DPRINT((CE_NOTE, "usbms_bot: op=0x%x CSW status=%d residue=%u\n",
		    cdb[0], csw[12], (unsigned)LE32_GET(csw, 8)));
		return (EIO);
	}
	return (0);
}

/* INQUIRY (36 bytes): log vendor/product so we know we're talking to it. */
static int
usbms_inquiry(struct usbms_softc *sc)
{
	unsigned char cdb[6];
	char vendor[9], product[17];
	int r, i;

	bzero(cdb, sizeof(cdb));
	cdb[0] = SCSI_INQUIRY;
	cdb[4] = 36;				/* allocation length */

	r = usbms_bot(sc, cdb, 6, 36, 1);
	if (r != 0)
		return (r);

	for (i = 0; i < 8; i++)  vendor[i]  = sc->sc_dma[8 + i];
	vendor[8] = '\0';
	for (i = 0; i < 16; i++) product[i] = sc->sc_dma[16 + i];
	product[16] = '\0';

	cmn_err(CE_NOTE, "usbms: INQUIRY vendor='%s' product='%s' type=0x%x\n",
	    vendor, product, sc->sc_dma[0] & 0x1f);
	return (0);
}

/*
 * REQUEST SENSE (18 bytes) - clears a pending CHECK CONDITION (notably the
 * power-on UNIT ATTENTION) and tells us why a command failed.  Returns the
 * sense key, or -1 on transport failure.
 */
static int
usbms_request_sense(struct usbms_softc *sc)
{
	unsigned char cdb[6];
	int key;

	bzero(cdb, sizeof(cdb));
	cdb[0] = SCSI_REQUEST_SENSE;
	cdb[4] = 18;				/* allocation length */

	if (usbms_bot(sc, cdb, 6, 18, 1) != 0)
		return (-1);

	key = sc->sc_dma[2] & 0x0f;		/* sense key */
	DPRINT((CE_NOTE, "usbms: SENSE key=0x%x ASC=0x%x ASCQ=0x%x\n",
	    key, sc->sc_dma[12], sc->sc_dma[13]));
	return (key);
}

/*
 * Wait for the unit to become ready.  A freshly enumerated stick reports
 * CHECK CONDITION / UNIT ATTENTION on first access; REQUEST SENSE clears it,
 * after which TEST UNIT READY succeeds.
 */
static int
usbms_test_unit_ready(struct usbms_softc *sc)
{
	unsigned char cdb[6];
	int r, tries;

	bzero(cdb, sizeof(cdb));
	cdb[0] = SCSI_TEST_UNIT_READY;

	for (tries = 0; tries < 20; tries++) {
		r = usbms_bot(sc, cdb, 6, 0, 0);
		if (r == 0)
			return (0);		/* ready */
		(void)usbms_request_sense(sc);	/* clear the condition */
		delay(HZ / 10);			/* 100 ms */
	}
	cmn_err(CE_WARN, "usbms: unit not ready after retries\n");
	return (r);
}

/* READ CAPACITY(10): last-LBA + block size, both big-endian. */
static int
usbms_read_capacity(struct usbms_softc *sc)
{
	unsigned char cdb[10];
	int r;
	__uint32_t last_lba, blksz;

	bzero(cdb, sizeof(cdb));
	cdb[0] = SCSI_READ_CAPACITY10;

	r = usbms_bot(sc, cdb, 10, 8, 1);
	if (r != 0)
		return (r);

	last_lba = BE32_GET(sc->sc_dma, 0);
	blksz    = BE32_GET(sc->sc_dma, 4);
	if (blksz == 0)
		blksz = USBMS_SECTOR;

	sc->sc_blksize = blksz;
	sc->sc_nblocks = (__uint64_t)last_lba + 1;

	cmn_err(CE_NOTE,
	    "usbms: CAPACITY %llu sectors x %u bytes = %llu MB\n",
	    sc->sc_nblocks, sc->sc_blksize,
	    (sc->sc_nblocks * sc->sc_blksize) >> 20);
	return (0);
}

/* READ(10) / WRITE(10) of nblk device-sectors at LBA into/out of sc_dma. */
static int
usbms_rw10(struct usbms_softc *sc, int write, __uint32_t lba, int nblk)
{
	unsigned char cdb[10];
	int bytes = nblk * sc->sc_blksize;

	bzero(cdb, sizeof(cdb));
	cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
	BE32_PUT(cdb, 2, lba);			/* logical block address */
	BE16_PUT(cdb, 7, nblk);			/* transfer length (blocks) */

	return (usbms_bot(sc, cdb, 10, bytes, write ? 0 : 1));
}

/*
 * Create /hw/usb/disk/<unit>/{char,block} and hang the softc off both.
 * (Block-device side mirrors the fl loopback example.)
 */
static int
usbms_hwgraph_create(struct usbms_softc *sc)
{
	char path[64];
	vertex_hdl_t base;

	sprintf(path, "usb/disk/%d", sc->sc_unit);

	if (hwgraph_path_add(hwgraph_root, path, &base) != GRAPH_SUCCESS) {
		cmn_err(CE_WARN, "usbms: hwgraph_path_add(%s) failed\n", path);
		return (ENXIO);
	}
	sc->sc_vtx_base = base;

	if (hwgraph_char_device_add(base, "char", USBMS_PREFIX,
	    &sc->sc_vtx_chr) != GRAPH_SUCCESS) {
		cmn_err(CE_WARN, "usbms: char_device_add failed\n");
		return (ENXIO);
	}
	if (hwgraph_block_device_add(base, "block", USBMS_PREFIX,
	    &sc->sc_vtx_blk) != GRAPH_SUCCESS) {
		cmn_err(CE_WARN, "usbms: block_device_add failed\n");
		return (ENXIO);
	}

	device_info_set(sc->sc_vtx_chr, sc);
	device_info_set(sc->sc_vtx_blk, sc);

	DPRINT((CE_NOTE, "usbms: created /hw/%s/{char,block}\n", path));
	return (0);
}

/*
 * Heavy, BLOCKING device bring-up.  Runs from usbms_open(), i.e. in the
 * calling process's context (sleepable) - NOT from the attach callback, which
 * the USB framework invokes in a non-sleepable context where large sleeping
 * allocations and blocking transfers (sv_wait) are illegal.
 * Idempotent via sc_ready.
 */
static int
usbms_setup(struct usbms_softc *sc)
{
	if (sc->sc_ready)
		return (0);

	TRACE("begin");

	/* DMA-able transfer buffers (usb_malloc, like the HID/audio drivers). */
	if (sc->sc_dma == NULL) {
		/* usb_malloc(size, a1, zero_select=1, kmflags=0/KM_SLEEP) */
		sc->sc_cbw = (unsigned char *)usb_malloc(CBW_LEN, 0, 1, 0);
		sc->sc_csw = (unsigned char *)usb_malloc(CSW_LEN, 0, 1, 0);
		sc->sc_dma = (unsigned char *)usb_malloc(USBMS_DMA_MAX, 0, 1, 0);
		if (sc->sc_cbw == NULL || sc->sc_csw == NULL || sc->sc_dma == NULL) {
			cmn_err(CE_WARN, "usbms_setup: usb_malloc failed\n");
			return (ENOMEM);
		}
	}
	TRACE("buffers ok");
	if (sc->sc_lock == NULL)
		sc->sc_lock = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, NULL);
	TRACE("mutex ok; opening bulk-IN");

	/* Open bulk IN/OUT pipes.  a1 = -1 selects "match by the endpoint
	 * descriptor passed in a2" (recovered: usb_hid_attach passes -1, same as
	 * usb_iface_defpipe).  Passing 0 returns the WRONG pipe and wedges the
	 * OHCI on the first transfer. */
	if (sc->sc_pipe_in == NULL &&
	    !USB_IS_OK(usb_pipe_open(sc->sc_iface, -1, sc->sc_ep_in,
		&sc->sc_pipe_in))) {
		cmn_err(CE_WARN, "usbms_setup: open bulk-IN failed\n");
		return (EIO);
	}
	TRACE("bulk-IN open; opening bulk-OUT");
	if (sc->sc_pipe_out == NULL &&
	    !USB_IS_OK(usb_pipe_open(sc->sc_iface, -1, sc->sc_ep_out,
		&sc->sc_pipe_out))) {
		cmn_err(CE_WARN, "usbms_setup: open bulk-OUT failed\n");
		return (EIO);
	}
	TRACE("pipes open; doing GET MAX LUN");

	usbms_get_max_lun(sc);
	TRACE("GET MAX LUN done; doing INQUIRY (first bulk)");

	/* First real bulk transfers. INQUIRY proves the BOT path; READ CAPACITY
	 * fills sc_blksize/sc_nblocks for the block device. */
	sc->sc_blksize = USBMS_SECTOR;		/* sane default */
	(void)usbms_inquiry(sc);
	TRACE("INQUIRY done; doing TEST UNIT READY");
	(void)usbms_test_unit_ready(sc);
	TRACE("TUR done; doing READ CAPACITY");
	if (usbms_read_capacity(sc) != 0)
		cmn_err(CE_WARN, "usbms_setup: READ CAPACITY failed "
		    "(size unknown)\n");
	TRACE("READ CAPACITY done");

	sc->sc_ready = 1;
	cmn_err(CE_NOTE, "usbms: unit %d ready: %llu x %u bytes (maxlun=%d)\n",
	    sc->sc_unit, sc->sc_nblocks, sc->sc_blksize, sc->sc_maxlun);
	return (0);
}

/*
 * ATTACH event.  The USB framework calls this in a NON-SLEEPABLE context, so we
 * do only light, non-blocking work here: stash the iface, read the bulk
 * endpoint addresses from the descriptors (no allocation), and create the
 * hwgraph device node (the same calls usb_hid_attach makes inline).  All
 * blocking bring-up is deferred to usbms_setup() on first open().
 */
static usb_status_t
usbms_attach(usb_iface_t iface)
{
	struct usbms_softc *sc;
	int unit;

	DPRINT((CE_NOTE, "usbms_attach: iface=0x%lx\n", (unsigned long)iface));

	for (unit = 0; unit < USBMS_MAXUNIT; unit++)
		if (usbms_units[unit] == NULL)
			break;
	if (unit == USBMS_MAXUNIT) {
		cmn_err(CE_WARN, "usbms_attach: no free unit slots\n");
		usbms_unclaim(iface);
		return (USB_EIO);
	}

	sc = (struct usbms_softc *)kmem_zalloc(sizeof(*sc), KM_NOSLEEP);
	if (sc == NULL) {
		usbms_unclaim(iface);
		return (USB_ENOMEM);
	}

	sc->sc_iface = iface;
	sc->sc_unit  = unit;

	if (usbms_find_endpoints(sc) != 0) {
		cmn_err(CE_WARN, "usbms_attach: no bulk endpoint pair\n");
		goto fail;
	}
	if (usbms_hwgraph_create(sc) != 0)
		goto fail;

	usbms_units[unit] = sc;
	cmn_err(CE_NOTE, "usbms: attached unit %d (bulk-IN=0x%x OUT=0x%x); "
	    "probe deferred to open()\n",
	    sc->sc_unit, sc->sc_addr_in, sc->sc_addr_out);
	return (USB_OK);

fail:
	usbms_unclaim(iface);
	kmem_free(sc, sizeof(*sc));
	return (USB_EIO);
}

/*
 * Tear down and free a softc.  Caller has already removed it from usbms_units.
 * Removes the hwgraph nodes, closes pipes, frees buffers/mutex/softc.
 */
static void
usbms_destroy(struct usbms_softc *sc)
{
	if (sc->sc_vtx_chr != 0) {
		device_info_set(sc->sc_vtx_chr, NULL);
		hwgraph_edge_remove(sc->sc_vtx_base, "char", NULL);
		hwgraph_vertex_unref(sc->sc_vtx_chr);
	}
	if (sc->sc_vtx_blk != 0) {
		device_info_set(sc->sc_vtx_blk, NULL);
		hwgraph_edge_remove(sc->sc_vtx_base, "block", NULL);
		hwgraph_vertex_unref(sc->sc_vtx_blk);
	}
	/*
	 * Do NOT hwgraph_vertex_unref(sc_vtx_base): the base (/hw/usb/disk/N)
	 * is still linked from its parent ("disk"); unref-ing it frees a vertex
	 * the parent points at -> corrupted hwgraph -> the next attach jumps to
	 * garbage (observed: misaligned-PC kernel fault).  We leave the base in
	 * place; re-attach reuses it (its char/block edges were just removed, so
	 * they re-add cleanly).
	 *
	 * Do NOT usb_pipe_close() either: the framework owns the pipes and frees
	 * them on disconnect (uaudio_detach closes nothing); doing it ourselves
	 * double-frees the pipe list (observed: TLB miss at 0x60 on re-attach).
	 */
	sc->sc_vtx_base = 0;

	if (sc->sc_cbw != NULL) usb_free(sc->sc_cbw);
	if (sc->sc_csw != NULL) usb_free(sc->sc_csw);
	if (sc->sc_dma != NULL) usb_free(sc->sc_dma);
	if (sc->sc_lock != NULL) MUTEX_DEALLOC(sc->sc_lock);

	kmem_free(sc, sizeof(*sc));
}

/*
 * DETACH event (USB unplug).  Unhook from the registry immediately so no new
 * opens find it.  If the device is still open (e.g. mounted), defer the actual
 * teardown to the final close so we don't free memory out from under in-flight
 * I/O; otherwise tear it down now.
 */
static usb_status_t
usbms_detach(usb_iface_t iface)
{
	struct usbms_softc *sc = usbms_find_by_iface(iface);

	DPRINT((CE_NOTE, "usbms_detach: iface=0x%lx sc=0x%lx\n",
	    (unsigned long)iface, (unsigned long)sc));

	usbms_unclaim(iface);

	if (sc == NULL)
		return (USB_OK);		/* not ours / already gone */

	usbms_units[sc->sc_unit] = NULL;	/* no new lookups */
	sc->sc_gone = 1;

	if (sc->sc_open > 0) {
		cmn_err(CE_NOTE, "usbms: unit %d detached while open; "
		    "teardown deferred to close\n", sc->sc_unit);
		return (USB_OK);
	}

	cmn_err(CE_NOTE, "usbms: detached unit %d\n", sc->sc_unit);
	usbms_destroy(sc);
	return (USB_OK);
}

/*
 * The single event handler registered with the USB core.  The core calls
 * this for every probe/attach/detach; ev->type selects the action and
 * ev->iface is the interface concerned (recovered ABI).
 */
static usb_status_t
usbms_event(struct usb_event *ev)
{
	switch (ev->type) {
	case USB_EVENT_PROBE:
		return (usbms_probe(ev->iface));
	case USB_EVENT_ATTACH:
		return (usbms_attach(ev->iface));
	case USB_EVENT_DETACH:
	case USB_EVENT_SUSPEND:
		return (usbms_detach(ev->iface));
	default:
		DPRINT((CE_NOTE, "usbms_event: unknown type %d\n", ev->type));
		return (USB_EINVAL);
	}
}

/* ================================================================== *
 *  Block / character device switch entry points (hwgraph dev_t = vertex).
 *  STUBS for the skeleton - the strategy data path is the next milestone.
 * ================================================================== */

/* ARGSUSED */
int
usbms_open(dev_t *devp, int oflag, int otyp, cred_t *crp)
{
	struct usbms_softc *sc =
	    (struct usbms_softc *)device_info_get((vertex_hdl_t)*devp);
	int r;

	if (sc == NULL || sc->sc_gone)
		return (ENXIO);

	/* Lazy, blocking bring-up in process context (sleepable).  sc_ready
	 * makes it idempotent. */
	r = usbms_setup(sc);
	if (r != 0)
		return (r);

	sc->sc_open++;
	DPRINT((CE_NOTE, "usbms_open: unit %d ready (open=%d)\n",
	    sc->sc_unit, sc->sc_open));
	return (0);
}

/* ARGSUSED */
int
usbms_close(dev_t dev, int oflag, int otyp, cred_t *crp)
{
	struct usbms_softc *sc =
	    (struct usbms_softc *)device_info_get((vertex_hdl_t)dev);

	if (sc == NULL)
		return (0);
	if (sc->sc_open > 0)
		sc->sc_open--;

	/* If the device was unplugged while open, complete the deferred teardown
	 * now that the last reference is gone. */
	if (sc->sc_open == 0 && sc->sc_gone)
		usbms_destroy(sc);
	return (0);
}

/*
 * Block I/O: translate bp into one or more SCSI READ(10)/WRITE(10) wrapped in
 * BOT, bouncing through sc_dma.  bp->b_blkno is in 512-byte (BBSIZE) units;
 * we map to device sectors of sc_blksize.  Serialised by sc_lock because the
 * CBW/CSW/DMA buffers and bulk pipes are shared and usb_bulk_trans_synch sleeps.
 */
void
usbms_strategy(struct buf *bp)
{
	struct usbms_softc *sc =
	    (struct usbms_softc *)device_info_get((vertex_hdl_t)bp->b_edev);
	int   write = !(bp->b_flags & B_READ);
	__uint64_t byteoff;
	__uint32_t lba;
	int   secsz, resid, chunk, nblk;
	int   mapped = 0;
	caddr_t addr;

	if (sc == NULL || sc->sc_gone) {
		bp->b_flags |= B_ERROR;
		bp->b_error = ENXIO;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}

	/*
	 * Lazily bring the device up.  The blocking SCSI bring-up (INQUIRY /
	 * TEST UNIT READY / READ CAPACITY) that fills in sc_blksize/sc_nblocks
	 * normally runs from usbms_open(). But a filesystem can issue
	 * buffer-cache reads (bread) against our dev_t WITHOUT first opening the
	 * block device. The FAT32 mount path does exactly this (lookupname +
	 * bread, no VOP_OPEN) - so open() never ran, the geometry is still zero,
	 * and the very first read of sector 0 used to fail with ENXIO.  Run setup
	 * here too: we're in the bread() caller's (sleepable) context, where this
	 * driver already sleeps for its bulk transfers.  Idempotent via sc_ready.
	 */
	if (!sc->sc_ready)
		(void)usbms_setup(sc);

	if (sc->sc_blksize == 0 || sc->sc_nblocks == 0) {
		bp->b_flags |= B_ERROR;
		bp->b_error = ENXIO;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}

	secsz   = sc->sc_blksize;
	byteoff = (__uint64_t)bp->b_blkno * USBMS_SECTOR;
	resid   = (int)bp->b_bcount;

	DPRINT((CE_NOTE, "usbms_strategy: %s blkno=%lld bcount=%d flags=0x%x\n",
	    (bp->b_flags & B_READ) ? "READ" : "WRITE",
	    (long long)bp->b_blkno, (int)bp->b_bcount, (unsigned)bp->b_flags));

	/* require whole-sector alignment */
	if ((byteoff % secsz) != 0 || (resid % secsz) != 0) {
		cmn_err(CE_WARN, "usbms_strategy: unaligned bcount=%d blkno=%lld "
		    "secsz=%d\n", (int)bp->b_bcount, (long long)bp->b_blkno, secsz);
		bp->b_flags |= B_ERROR;
		bp->b_error = EINVAL;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}
	lba = (__uint32_t)(byteoff / secsz);

	/* Get a kernel address for the buffer data.  File-data I/O uses page-I/O
	 * buffers (B_PAGEIO): the data is described by b_pages, and bp_mapin()
	 * maps that page list into kernel VA and RETURNS the address (bp_mapout
	 * releases it).  Metadata/raw bufs (no B_PAGEIO, incl. all mount I/O)
	 * already carry a valid b_un.b_addr and are left untouched. */
	if (bp->b_flags & B_PAGEIO) {
		addr = (caddr_t)bp_mapin(bp);
		mapped = 1;
	} else {
		addr = bp->b_un.b_addr;
	}
	if (addr == NULL) {
		cmn_err(CE_WARN, "usbms_strategy: no buffer address, flags=0x%x\n",
		    (unsigned)bp->b_flags);
		if (mapped)
			bp_mapout(bp);
		bp->b_flags |= B_ERROR;
		bp->b_error = EIO;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}

	/* Serialise: the CBW/CSW/DMA buffers and bulk pipes are shared.  Holding
	 * this sleep-mutex across the blocking transfers is fine - the framework
	 * itself does mutex_lock + sv_wait. */
	MUTEX_LOCK(sc->sc_lock, -1);
	while (resid > 0) {
		chunk = (resid > USBMS_XFER_MAX) ? USBMS_XFER_MAX : resid;
		nblk  = chunk / secsz;

		if ((__uint64_t)lba + nblk > sc->sc_nblocks) {
			bp->b_flags |= B_ERROR;
			bp->b_error = ENXIO;
			break;
		}

		if (write) {
			bcopy(addr, sc->sc_dma, chunk);
			if (usbms_rw10(sc, 1, lba, nblk) != 0) {
				bp->b_flags |= B_ERROR;
				bp->b_error = EIO;
				break;
			}
		} else {
			if (usbms_rw10(sc, 0, lba, nblk) != 0) {
				bp->b_flags |= B_ERROR;
				bp->b_error = EIO;
				break;
			}
			bcopy(sc->sc_dma, addr, chunk);
		}

		addr  += chunk;
		lba   += nblk;
		resid -= chunk;
	}
	MUTEX_UNLOCK(sc->sc_lock);

	if (mapped)
		bp_mapout(bp);

	bp->b_resid = resid;
	biodone(bp);
}

/*
 * Raw character read/write - drive usbms_strategy() via uiophysio() (the dev_t
 * for a hwgraph device is its vertex handle, which strategy resolves with
 * device_info_get()).
 */
/* ARGSUSED */
int
usbms_read(dev_t dev, uio_t *uio, cred_t *crp)
{
	DPRINT((CE_NOTE, "usbms_read: resid=%d off=%lld\n",
	    (int)uio->uio_resid, (long long)uio->uio_offset));
	return (uiophysio((int (*)(struct buf *))usbms_strategy, NULL,
	    dev, B_READ, uio));
}

/* ARGSUSED */
int
usbms_write(dev_t dev, uio_t *uio, cred_t *crp)
{
	DPRINT((CE_NOTE, "usbms_write: resid=%d off=%lld\n",
	    (int)uio->uio_resid, (long long)uio->uio_offset));
	return (uiophysio((int (*)(struct buf *))usbms_strategy, NULL,
	    dev, B_WRITE, uio));
}

/*
 * Device size in 512-byte blocks.  Until READ CAPACITY is wired, report 0.
 */
int
usbms_size(dev_t dev)
{
	struct usbms_softc *sc =
	    (struct usbms_softc *)device_info_get((vertex_hdl_t)dev);

	if (sc == NULL || sc->sc_blksize == 0)
		return (0);
	/* report in 512-byte (BBSIZE) units regardless of device sector size */
	return ((int)((sc->sc_nblocks * sc->sc_blksize) / USBMS_SECTOR));
}

/* ARGSUSED */
int
usbms_ioctl(dev_t dev, int cmd, void *arg, int mode, cred_t *crp, int *rvalp)
{
	/* Log what callers (mkfs/mount/mount_dos) probe us with, so we know which
	 * disk ioctls to implement.  Still returns EINVAL for now. */
	cmn_err(CE_NOTE, "usbms_ioctl: cmd=0x%x ('%c'=%d) arg=0x%lx mode=0x%x\n",
	    cmd, (cmd >> 8) & 0xff, cmd & 0xff, (unsigned long)arg, mode);
	return (EINVAL);
}

/* ================================================================== *
 *  Loadable-module lifecycle.
 * ================================================================== */

int
usbms_init(void)
{
	usb_status_t st;

	st = usb_driver_register(usbms_event, "usbms", NULL);
	if (!USB_IS_OK(st)) {
		cmn_err(CE_WARN, "usbms_init: usb_driver_register failed 0x%x\n",
		    (unsigned)st);
		return (EIO);
	}
	cmn_err(CE_NOTE, "usbms: registered with USB core (mass-storage BBB)\n");
	return (0);
}

/* ARGSUSED */
int
usbms_reg(void)
{
	return (0);
}

/* ARGSUSED */
int
usbms_unreg(void)
{
	return (0);
}

int
usbms_unload(void)
{
	usb_driver_unregister(usbms_event, "usbms", NULL);
	cmn_err(CE_NOTE, "usbms: unregistered\n");
	return (0);
}
