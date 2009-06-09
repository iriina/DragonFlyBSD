/*
 * Copyright (c) 2006 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $OpenBSD: ahci.c,v 1.147 2009/02/16 21:19:07 miod Exp $
 */

#if defined(__DragonFly__)
#include "ahci_dragonfly.h"
#else
#error "build for OS unknown"
#endif
#include "atascsi.h"

/* change to AHCI_DEBUG for dmesg spam */
#define NO_AHCI_DEBUG

#ifdef AHCI_DEBUG
#define DPRINTF(m, f...) do { if ((ahcidebug & (m)) == (m)) kprintf(f); } \
    while (0)
#define AHCI_D_TIMEOUT		0x00
#define AHCI_D_VERBOSE		0x01
#define AHCI_D_INTR		0x02
#define AHCI_D_XFER		0x08
int ahcidebug = AHCI_D_VERBOSE;
#else
#define DPRINTF(m, f...)
#endif

#define AHCI_PCI_BAR		0x24
#define AHCI_PCI_ATI_SB600_MAGIC	0x40
#define AHCI_PCI_ATI_SB600_LOCKED	0x01
#define AHCI_PCI_INTERFACE	0x01

#define AHCI_REG_CAP		0x000 /* HBA Capabilities */
#define  AHCI_REG_CAP_NP(_r)		(((_r) & 0x1f)+1) /* Number of Ports */
#define  AHCI_REG_CAP_SXS		(1<<5) /* External SATA */
#define  AHCI_REG_CAP_EMS		(1<<6) /* Enclosure Mgmt */
#define  AHCI_REG_CAP_CCCS		(1<<7) /* Cmd Coalescing */
#define  AHCI_REG_CAP_NCS(_r)		((((_r) & 0x1f00)>>8)+1) /* NCmds*/
#define  AHCI_REG_CAP_PSC		(1<<13) /* Partial State Capable */
#define  AHCI_REG_CAP_SSC		(1<<14) /* Slumber State Capable */
#define  AHCI_REG_CAP_PMD		(1<<15) /* PIO Multiple DRQ Block */
#define  AHCI_REG_CAP_FBSS		(1<<16) /* FIS-Based Switching */
#define  AHCI_REG_CAP_SPM		(1<<17) /* Port Multiplier */
#define  AHCI_REG_CAP_SAM		(1<<18) /* AHCI Only mode */
#define  AHCI_REG_CAP_SNZO		(1<<19) /* Non Zero DMA Offsets */
#define  AHCI_REG_CAP_ISS		(0xf<<20) /* Interface Speed Support */
#define  AHCI_REG_CAP_ISS_G1		(0x1<<20) /* Gen 1 (1.5 Gbps) */
#define  AHCI_REG_CAP_ISS_G1_2		(0x2<<20) /* Gen 1 and 2 (3 Gbps) */
#define  AHCI_REG_CAP_SCLO		(1<<24) /* Cmd List Override */
#define  AHCI_REG_CAP_SAL		(1<<25) /* Activity LED */
#define  AHCI_REG_CAP_SALP		(1<<26) /* Aggressive Link Pwr Mgmt */
#define  AHCI_REG_CAP_SSS		(1<<27) /* Staggered Spinup */
#define  AHCI_REG_CAP_SMPS		(1<<28) /* Mech Presence Switch */
#define  AHCI_REG_CAP_SSNTF		(1<<29) /* SNotification Register */
#define  AHCI_REG_CAP_SNCQ		(1<<30) /* Native Cmd Queuing */
#define  AHCI_REG_CAP_S64A		(1<<31) /* 64bit Addressing */
#define  AHCI_FMT_CAP		"\020" "\040S64A" "\037NCQ" "\036SSNTF" \
				    "\035SMPS" "\034SSS" "\033SALP" "\032SAL" \
				    "\031SCLO" "\024SNZO" "\023SAM" "\022SPM" \
				    "\021FBSS" "\020PMD" "\017SSC" "\016PSC" \
				    "\010CCCS" "\007EMS" "\006SXS"

#define AHCI_REG_GHC		0x004 /* Global HBA Control */
#define  AHCI_REG_GHC_HR		(1<<0) /* HBA Reset */
#define  AHCI_REG_GHC_IE		(1<<1) /* Interrupt Enable */
#define  AHCI_REG_GHC_MRSM		(1<<2) /* MSI Revert to Single Msg */
#define  AHCI_REG_GHC_AE		(1<<31) /* AHCI Enable */
#define AHCI_FMT_GHC		"\020" "\040AE" "\003MRSM" "\002IE" "\001HR"

#define AHCI_REG_IS		0x008 /* Interrupt Status */
#define AHCI_REG_PI		0x00c /* Ports Implemented */

#define AHCI_REG_VS		0x010 /* AHCI Version */
#define  AHCI_REG_VS_0_95		0x00000905 /* 0.95 */
#define  AHCI_REG_VS_1_0		0x00010000 /* 1.0 */
#define  AHCI_REG_VS_1_1		0x00010100 /* 1.1 */
#define  AHCI_REG_VS_1_2		0x00010200 /* 1.2 */

#define AHCI_REG_CCC_CTL	0x014 /* Coalescing Control */
#define  AHCI_REG_CCC_CTL_INT(_r)	(((_r) & 0xf8) >> 3) /* CCC INT slot */

#define AHCI_REG_CCC_PORTS	0x018 /* Coalescing Ports */
#define AHCI_REG_EM_LOC		0x01c /* Enclosure Mgmt Location */
#define AHCI_REG_EM_CTL		0x020 /* Enclosure Mgmt Control */

#define AHCI_PORT_REGION(_p)	(0x100 + ((_p) * 0x80))
#define AHCI_PORT_SIZE		0x80

#define AHCI_PREG_CLB		0x00 /* Cmd List Base Addr */
#define AHCI_PREG_CLBU		0x04 /* Cmd List Base Hi Addr */
#define AHCI_PREG_FB		0x08 /* FIS Base Addr */
#define AHCI_PREG_FBU		0x0c /* FIS Base Hi Addr */

#define AHCI_PREG_IS		0x10 /* Interrupt Status */
#define  AHCI_PREG_IS_DHRS		(1<<0) /* Device to Host FIS */
#define  AHCI_PREG_IS_PSS		(1<<1) /* PIO Setup FIS */
#define  AHCI_PREG_IS_DSS		(1<<2) /* DMA Setup FIS */
#define  AHCI_PREG_IS_SDBS		(1<<3) /* Set Device Bits FIS */
#define  AHCI_PREG_IS_UFS		(1<<4) /* Unknown FIS */
#define  AHCI_PREG_IS_DPS		(1<<5) /* Descriptor Processed */
#define  AHCI_PREG_IS_PCS		(1<<6) /* Port Change */
#define  AHCI_PREG_IS_DMPS		(1<<7) /* Device Mechanical Presence */
#define  AHCI_PREG_IS_PRCS		(1<<22) /* PhyRdy Change */
#define  AHCI_PREG_IS_IPMS		(1<<23) /* Incorrect Port Multiplier */
#define  AHCI_PREG_IS_OFS		(1<<24) /* Overflow */
#define  AHCI_PREG_IS_INFS		(1<<26) /* Interface Non-fatal Error */
#define  AHCI_PREG_IS_IFS		(1<<27) /* Interface Fatal Error */
#define  AHCI_PREG_IS_HBDS		(1<<28) /* Host Bus Data Error */
#define  AHCI_PREG_IS_HBFS		(1<<29) /* Host Bus Fatal Error */
#define  AHCI_PREG_IS_TFES		(1<<30) /* Task File Error */
#define  AHCI_PREG_IS_CPDS		(1<<31) /* Cold Presence Detect */
#define AHCI_PFMT_IS		"\20" "\040CPDS" "\037TFES" "\036HBFS" \
				    "\035HBDS" "\034IFS" "\033INFS" "\031OFS" \
				    "\030IPMS" "\027PRCS" "\010DMPS" "\006DPS" \
				    "\007PCS" "\005UFS" "\004SDBS" "\003DSS" \
				    "\002PSS" "\001DHRS"

#define AHCI_PREG_IE		0x14 /* Interrupt Enable */
#define  AHCI_PREG_IE_DHRE		(1<<0) /* Device to Host FIS */
#define  AHCI_PREG_IE_PSE		(1<<1) /* PIO Setup FIS */
#define  AHCI_PREG_IE_DSE		(1<<2) /* DMA Setup FIS */
#define  AHCI_PREG_IE_SDBE		(1<<3) /* Set Device Bits FIS */
#define  AHCI_PREG_IE_UFE		(1<<4) /* Unknown FIS */
#define  AHCI_PREG_IE_DPE		(1<<5) /* Descriptor Processed */
#define  AHCI_PREG_IE_PCE		(1<<6) /* Port Change */
#define  AHCI_PREG_IE_DMPE		(1<<7) /* Device Mechanical Presence */
#define  AHCI_PREG_IE_PRCE		(1<<22) /* PhyRdy Change */
#define  AHCI_PREG_IE_IPME		(1<<23) /* Incorrect Port Multiplier */
#define  AHCI_PREG_IE_OFE		(1<<24) /* Overflow */
#define  AHCI_PREG_IE_INFE		(1<<26) /* Interface Non-fatal Error */
#define  AHCI_PREG_IE_IFE		(1<<27) /* Interface Fatal Error */
#define  AHCI_PREG_IE_HBDE		(1<<28) /* Host Bus Data Error */
#define  AHCI_PREG_IE_HBFE		(1<<29) /* Host Bus Fatal Error */
#define  AHCI_PREG_IE_TFEE		(1<<30) /* Task File Error */
#define  AHCI_PREG_IE_CPDE		(1<<31) /* Cold Presence Detect */
#define AHCI_PFMT_IE		"\20" "\040CPDE" "\037TFEE" "\036HBFE" \
				    "\035HBDE" "\034IFE" "\033INFE" "\031OFE" \
				    "\030IPME" "\027PRCE" "\010DMPE" "\007PCE" \
				    "\006DPE" "\005UFE" "\004SDBE" "\003DSE" \
				    "\002PSE" "\001DHRE"

#define AHCI_PREG_CMD		0x18 /* Command and Status */
#define  AHCI_PREG_CMD_ST		(1<<0) /* Start */
#define  AHCI_PREG_CMD_SUD		(1<<1) /* Spin Up Device */
#define  AHCI_PREG_CMD_POD		(1<<2) /* Power On Device */
#define  AHCI_PREG_CMD_CLO		(1<<3) /* Command List Override */
#define  AHCI_PREG_CMD_FRE		(1<<4) /* FIS Receive Enable */
#define  AHCI_PREG_CMD_CCS(_r)		(((_r) >> 8) & 0x1f) /* Curr CmdSlot# */
#define  AHCI_PREG_CMD_MPSS		(1<<13) /* Mech Presence State */
#define  AHCI_PREG_CMD_FR		(1<<14) /* FIS Receive Running */
#define  AHCI_PREG_CMD_CR		(1<<15) /* Command List Running */
#define  AHCI_PREG_CMD_CPS		(1<<16) /* Cold Presence State */
#define  AHCI_PREG_CMD_PMA		(1<<17) /* Port Multiplier Attached */
#define  AHCI_PREG_CMD_HPCP		(1<<18) /* Hot Plug Capable */
#define  AHCI_PREG_CMD_MPSP		(1<<19) /* Mech Presence Switch */
#define  AHCI_PREG_CMD_CPD		(1<<20) /* Cold Presence Detection */
#define  AHCI_PREG_CMD_ESP		(1<<21) /* External SATA Port */
#define  AHCI_PREG_CMD_ATAPI		(1<<24) /* Device is ATAPI */
#define  AHCI_PREG_CMD_DLAE		(1<<25) /* Drv LED on ATAPI Enable */
#define  AHCI_PREG_CMD_ALPE		(1<<26) /* Aggro Pwr Mgmt Enable */
#define  AHCI_PREG_CMD_ASP		(1<<27) /* Aggro Slumber/Partial */
#define  AHCI_PREG_CMD_ICC		0xf0000000 /* Interface Comm Ctrl */
#define  AHCI_PREG_CMD_ICC_SLUMBER	0x60000000
#define  AHCI_PREG_CMD_ICC_PARTIAL	0x20000000
#define  AHCI_PREG_CMD_ICC_ACTIVE	0x10000000
#define  AHCI_PREG_CMD_ICC_IDLE		0x00000000
#define  AHCI_PFMT_CMD		"\020" "\034ASP" "\033ALPE" "\032DLAE" \
				    "\031ATAPI" "\026ESP" "\025CPD" "\024MPSP" \
				    "\023HPCP" "\022PMA" "\021CPS" "\020CR" \
				    "\017FR" "\016MPSS" "\005FRE" "\004CLO" \
				    "\003POD" "\002SUD" "\001ST"

#define AHCI_PREG_TFD		0x20 /* Task File Data*/
#define  AHCI_PREG_TFD_STS		0xff
#define  AHCI_PREG_TFD_STS_ERR		(1<<0)
#define  AHCI_PREG_TFD_STS_DRQ		(1<<3)
#define  AHCI_PREG_TFD_STS_BSY		(1<<7)
#define  AHCI_PREG_TFD_ERR		0xff00

#define AHCI_PFMT_TFD_STS	"\20" "\010BSY" "\004DRQ" "\001ERR"
#define AHCI_PREG_SIG		0x24 /* Signature */

#define AHCI_PREG_SSTS		0x28 /* SATA Status */
#define  AHCI_PREG_SSTS_DET		0xf /* Device Detection */
#define  AHCI_PREG_SSTS_DET_NONE	0x0
#define  AHCI_PREG_SSTS_DET_DEV_NE	0x1
#define  AHCI_PREG_SSTS_DET_DEV		0x3
#define  AHCI_PREG_SSTS_DET_PHYOFFLINE	0x4
#define  AHCI_PREG_SSTS_SPD		0xf0 /* Current Interface Speed */
#define  AHCI_PREG_SSTS_SPD_NONE	0x00
#define  AHCI_PREG_SSTS_SPD_GEN1	0x10
#define  AHCI_PREG_SSTS_SPD_GEN2	0x20
#define  AHCI_PREG_SSTS_IPM		0xf00 /* Interface Power Management */
#define  AHCI_PREG_SSTS_IPM_NONE	0x000
#define  AHCI_PREG_SSTS_IPM_ACTIVE	0x100
#define  AHCI_PREG_SSTS_IPM_PARTIAL	0x200
#define  AHCI_PREG_SSTS_IPM_SLUMBER	0x600

#define AHCI_PREG_SCTL		0x2c /* SATA Control */
#define  AHCI_PREG_SCTL_DET		0xf /* Device Detection */
#define  AHCI_PREG_SCTL_DET_NONE	0x0
#define  AHCI_PREG_SCTL_DET_INIT	0x1
#define  AHCI_PREG_SCTL_DET_DISABLE	0x4
#define  AHCI_PREG_SCTL_SPD		0xf0 /* Speed Allowed */
#define  AHCI_PREG_SCTL_SPD_ANY		0x00
#define  AHCI_PREG_SCTL_SPD_GEN1	0x10
#define  AHCI_PREG_SCTL_SPD_GEN2	0x20
#define  AHCI_PREG_SCTL_IPM		0xf00 /* Interface Power Management */
#define  AHCI_PREG_SCTL_IPM_NONE	0x000
#define  AHCI_PREG_SCTL_IPM_NOPARTIAL	0x100
#define  AHCI_PREG_SCTL_IPM_NOSLUMBER	0x200
#define  AHCI_PREG_SCTL_IPM_DISABLED	0x300

#define AHCI_PREG_SERR		0x30 /* SATA Error */
#define  AHCI_PREG_SERR_ERR(_r)		((_r) & 0xffff)
#define  AHCI_PREG_SERR_ERR_I		(1<<0) /* Recovered Data Integrity */
#define  AHCI_PREG_SERR_ERR_M		(1<<1) /* Recovered Communications */
#define  AHCI_PREG_SERR_ERR_T		(1<<8) /* Transient Data Integrity */
#define  AHCI_PREG_SERR_ERR_C		(1<<9) /* Persistent Comm/Data */
#define  AHCI_PREG_SERR_ERR_P		(1<<10) /* Protocol */
#define  AHCI_PREG_SERR_ERR_E		(1<<11) /* Internal */
#define  AHCI_PFMT_SERR_ERR	"\020" "\014E" "\013P" "\012C" "\011T" "\002M" \
				    "\001I"
#define  AHCI_PREG_SERR_DIAG(_r)	(((_r) >> 16) & 0xffff)
#define  AHCI_PREG_SERR_DIAG_N		(1<<0) /* PhyRdy Change */
#define  AHCI_PREG_SERR_DIAG_I		(1<<1) /* Phy Internal Error */
#define  AHCI_PREG_SERR_DIAG_W		(1<<2) /* Comm Wake */
#define  AHCI_PREG_SERR_DIAG_B		(1<<3) /* 10B to 8B Decode Error */
#define  AHCI_PREG_SERR_DIAG_D		(1<<4) /* Disparity Error */
#define  AHCI_PREG_SERR_DIAG_C		(1<<5) /* CRC Error */
#define  AHCI_PREG_SERR_DIAG_H		(1<<6) /* Handshake Error */
#define  AHCI_PREG_SERR_DIAG_S		(1<<7) /* Link Sequence Error */
#define  AHCI_PREG_SERR_DIAG_T		(1<<8) /* Transport State Trans Err */
#define  AHCI_PREG_SERR_DIAG_F		(1<<9) /* Unknown FIS Type */
#define  AHCI_PREG_SERR_DIAG_X		(1<<10) /* Exchanged */
#define  AHCI_PFMT_SERR_DIAG	"\020" "\013X" "\012F" "\011T" "\010S" "\007H" \
				    "\006C" "\005D" "\004B" "\003W" "\002I" \
				    "\001N"

#define AHCI_PREG_SACT		0x34 /* SATA Active */
#define AHCI_PREG_CI		0x38 /* Command Issue */
#define  AHCI_PREG_CI_ALL_SLOTS	0xffffffff
#define AHCI_PREG_SNTF		0x3c /* SNotification */

struct ahci_cmd_hdr {
	u_int16_t		flags;
#define AHCI_CMD_LIST_FLAG_CFL		0x001f /* Command FIS Length */
#define AHCI_CMD_LIST_FLAG_A		(1<<5) /* ATAPI */
#define AHCI_CMD_LIST_FLAG_W		(1<<6) /* Write */
#define AHCI_CMD_LIST_FLAG_P		(1<<7) /* Prefetchable */
#define AHCI_CMD_LIST_FLAG_R		(1<<8) /* Reset */
#define AHCI_CMD_LIST_FLAG_B		(1<<9) /* BIST */
#define AHCI_CMD_LIST_FLAG_C		(1<<10) /* Clear Busy upon R_OK */
#define AHCI_CMD_LIST_FLAG_PMP		0xf000 /* Port Multiplier Port */
	u_int16_t		prdtl; /* sgl len */

	u_int32_t		prdbc; /* transferred byte count */

	u_int32_t		ctba_lo;
	u_int32_t		ctba_hi;

	u_int32_t		reserved[4];
} __packed;

struct ahci_rfis {
	u_int8_t		dsfis[28];
	u_int8_t		reserved1[4];
	u_int8_t		psfis[24];
	u_int8_t		reserved2[8];
	u_int8_t		rfis[24];
	u_int8_t		reserved3[4];
	u_int8_t		sdbfis[4];
	u_int8_t		ufis[64];
	u_int8_t		reserved4[96];
} __packed;

struct ahci_prdt {
	u_int32_t		dba_lo;
	u_int32_t		dba_hi;
	u_int32_t		reserved;
	u_int32_t		flags;
#define AHCI_PRDT_FLAG_INTR		(1<<31) /* interrupt on completion */
} __packed;

/*
 * The base command table structure is 128 bytes.  Each prdt is 16 bytes.
 * We need to accomodate MAXPHYS (128K) which is at least 32 entries,
 * plus one for page slop.
 *
 * Making the ahci_cmd_table 1024 bytes (a reasonable power of 2)
 * thus requires MAX_PRDT to be set to 56.
 */
#define AHCI_MAX_PRDT		56

#if MAXPHYS / PAGE_SIZE + 1 > AHCI_MAX_PRDT
#error "AHCI_MAX_PRDT is not big enough"
#endif

struct ahci_cmd_table {
	u_int8_t		cfis[64];	/* Command FIS */
	u_int8_t		acmd[16];	/* ATAPI Command */
	u_int8_t		reserved[48];

	struct ahci_prdt	prdt[AHCI_MAX_PRDT];
} __packed;

#define AHCI_MAX_PORTS		32

struct ahci_dmamem {
	bus_dma_tag_t		adm_tag;
	bus_dmamap_t		adm_map;
	bus_dma_segment_t	adm_seg;
	bus_addr_t		adm_busaddr;
	caddr_t			adm_kva;
};
#define AHCI_DMA_MAP(_adm)	((_adm)->adm_map)
#define AHCI_DMA_DVA(_adm)	((_adm)->adm_busaddr)
#define AHCI_DMA_KVA(_adm)	((void *)(_adm)->adm_kva)

struct ahci_softc;
struct ahci_port;
struct ahci_device;

struct ahci_ccb {
	/* ATA xfer associated with this CCB.  Must be 1st struct member. */
	struct ata_xfer		ccb_xa;
	struct callout          ccb_timeout;

	int			ccb_slot;
	struct ahci_port	*ccb_port;

	bus_dmamap_t		ccb_dmamap;
	struct ahci_cmd_hdr	*ccb_cmd_hdr;
	struct ahci_cmd_table	*ccb_cmd_table;

	void			(*ccb_done)(struct ahci_ccb *);

	TAILQ_ENTRY(ahci_ccb)	ccb_entry;
};

struct ahci_port {
	struct ahci_softc	*ap_sc;
	bus_space_handle_t	ap_ioh;

	int			ap_num;
	int			ap_flags;
#define AP_F_BUS_REGISTERED	0x0001
#define AP_F_CAM_ATTACHED	0x0002
	struct cam_sim		*ap_sim;
	struct cam_path		*ap_path;

	struct ahci_rfis	*ap_rfis;
	struct ahci_dmamem	*ap_dmamem_rfis;

	struct ahci_dmamem	*ap_dmamem_cmd_list;
	struct ahci_dmamem	*ap_dmamem_cmd_table;

	volatile u_int32_t	ap_active;
	volatile u_int32_t	ap_active_cnt;
	volatile u_int32_t	ap_sactive;
	struct ahci_ccb		*ap_ccbs;

	TAILQ_HEAD(, ahci_ccb)	ap_ccb_free;
	TAILQ_HEAD(, ahci_ccb)	ap_ccb_pending;
	struct lock		ap_ccb_lock;

	struct ata_port		ap_ata;

	u_int32_t		ap_state;
#define AP_S_NORMAL			0
#define AP_S_FATAL_ERROR		1

	/* For error recovery. */
#ifdef DIAGNOSTIC
	int			ap_err_busy;
#endif
	u_int32_t		ap_err_saved_sactive;
	u_int32_t		ap_err_saved_active;
	u_int32_t		ap_err_saved_active_cnt;

	u_int8_t		ap_err_scratch[512];

	char			ap_name[16];
};

#define PORTNAME(_ap)	((_ap)->ap_name)

struct ahci_softc {
	device_t		sc_dev;
	const struct ahci_device *sc_ad;	/* special casing */
	struct lwkt_serialize	sc_serializer;

	struct resource		*sc_irq;	/* bus resources */
	struct resource		*sc_regs;	/* bus resources */
	bus_space_tag_t		sc_iot;		/* split from sc_regs */
	bus_space_handle_t	sc_ioh;		/* split from sc_regs */

	int			sc_rid_irq;	/* saved bus RIDs */
	int			sc_rid_regs;
	u_int32_t		sc_cap;		/* capabilities */

	void			*sc_irq_handle;	/* installed irq vector */

	bus_dma_tag_t		sc_tag_rfis;	/* bus DMA tags */
	bus_dma_tag_t		sc_tag_cmdh;
	bus_dma_tag_t		sc_tag_cmdt;
	bus_dma_tag_t		sc_tag_data;

	int			sc_flags;
#define AHCI_F_NO_NCQ			(1<<0)
#define AHCI_F_IGN_FR			(1<<1)

	u_int			sc_ncmds;

	struct ahci_port	*sc_ports[AHCI_MAX_PORTS];

#ifdef AHCI_COALESCE
	u_int32_t		sc_ccc_mask;
	u_int32_t		sc_ccc_ports;
	u_int32_t		sc_ccc_ports_cur;
#endif
};
#define DEVNAME(_s)		((_s)->sc_dev.dv_xname)

struct ahci_device {
	pci_vendor_id_t		ad_vendor;
	pci_product_id_t	ad_product;
	int			(*ad_attach)(device_t dev);
	int			(*ad_detach)(device_t dev);
	char			*name;
};

const struct ahci_device *ahci_lookup_device(device_t dev);
int	ahci_init(struct ahci_softc *);
int	ahci_port_alloc(struct ahci_softc *, u_int);
void	ahci_port_free(struct ahci_softc *, u_int);
int	ahci_port_reset(struct ahci_port *, int);

u_int32_t ahci_read(struct ahci_softc *, bus_size_t);
void	ahci_write(struct ahci_softc *, bus_size_t, u_int32_t);
int	ahci_wait_ne(struct ahci_softc *, bus_size_t, u_int32_t, u_int32_t);
u_int32_t ahci_pread(struct ahci_port *, bus_size_t);
void	ahci_pwrite(struct ahci_port *, bus_size_t, u_int32_t);
int	ahci_pwait_eq(struct ahci_port *, int, bus_size_t,
			u_int32_t, u_int32_t);
void	ahci_intr(void *);
u_int32_t ahci_port_intr(struct ahci_port *, u_int32_t);

int	ahci_cam_attach(struct ahci_port *ap);
void	ahci_cam_changed(struct ahci_port *ap, int found);
void	ahci_cam_detach(struct ahci_port *ap);

struct ata_xfer *ahci_ata_get_xfer(struct ahci_port *ap);
void ahci_ata_put_xfer(struct ata_xfer *xa);
int ahci_ata_cmd(struct ata_xfer *xa);

extern u_int32_t AhciForceGen1;