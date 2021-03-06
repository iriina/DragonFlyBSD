/*
 * Copyright (c) 2006 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Sepherosa Ziehau <sepherosa@gmail.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_media.h>

#include <netproto/802_11/ieee80211_var.h>
#include <netproto/802_11/ieee80211_radiotap.h>
#include <netproto/802_11/wlan_ratectl/amrr/ieee80211_amrr_param.h>
#include <netproto/802_11/wlan_ratectl/onoe/ieee80211_onoe_param.h>

#include <bus/pci/pcireg.h>

#define ACX_DEBUG

#include <dev/netif/acx/if_acxreg.h>
#include <dev/netif/acx/if_acxvar.h>
#include <dev/netif/acx/acxcmd.h>

#define ACX100_CONF_FW_RING	0x0003
#define ACX100_CONF_MEMOPT	0x0005

#define ACX100_INTR_ENABLE	(ACXRV_INTR_TX_FINI | ACXRV_INTR_RX_FINI)
/*
 * XXX do we really care about following interrupts?
 *
 * ACXRV_INTR_INFO | ACXRV_INTR_SCAN_FINI
 */

#define ACX100_INTR_DISABLE	(uint16_t)~(ACXRV_INTR_UNKN)

#define ACX100_RATE(rate)	((rate) * 5)

#define ACX100_RSSI_CORR	8
#define ACX100_TXPOWER		18
#define ACX100_GPIO_POWER_LED	0x0800
#define ACX100_EE_EADDR_OFS	0x1a

#define ACX100_FW_TXRING_SIZE	(ACX_TX_DESC_CNT * sizeof(struct acx_fw_txdesc))
#define ACX100_FW_RXRING_SIZE	(ACX_RX_DESC_CNT * sizeof(struct acx_fw_rxdesc))

/*
 * NOTE:
 * Following structs' fields are little endian
 */

struct acx100_bss_join {
	uint8_t	dtim_intvl;
	uint8_t	basic_rates;
	uint8_t	op_rates;
} __packed;

struct acx100_conf_fw_ring {
	struct acx_conf	confcom;
	uint32_t	fw_ring_size;	/* total size of fw (tx + rx) ring */
	uint32_t	fw_rxring_addr;	/* start phyaddr of fw rx desc */
	uint8_t		opt;		/* see ACX100_RINGOPT_ */
	uint8_t		fw_txring_num;	/* num of TX ring */
	uint8_t		fw_rxdesc_num;	/* num of fw rx desc */
	uint8_t		reserved0;
	uint32_t	fw_ring_end[2];	/* see ACX100_SET_RING_END() */
	uint32_t	fw_txring_addr;	/* start phyaddr of fw tx desc */
	uint8_t		fw_txring_prio;	/* see ACX100_TXRING_PRIO_ */
	uint8_t		fw_txdesc_num;	/* num of fw tx desc */
	uint16_t	reserved1;
} __packed;

#define ACX100_RINGOPT_AUTO_RESET	0x1
#define ACX100_TXRING_PRIO_DEFAULT	0
#define ACX100_SET_RING_END(conf, end)			\
do {							\
	(conf)->fw_ring_end[0] = htole32(end);		\
	(conf)->fw_ring_end[1] = htole32(end + 8);	\
} while (0)

struct acx100_conf_memblk_size {
	struct acx_conf	confcom;
	uint16_t	memblk_size;	/* size of each mem block */
} __packed;

struct acx100_conf_mem {
	struct acx_conf	confcom;
	uint32_t	opt;		/* see ACX100_MEMOPT_ */
	uint32_t	h_rxring_paddr;	/* host rx desc start phyaddr */

	/*
	 * Memory blocks are controled by hardware
	 * once after they are initialized
	 */
	uint32_t	rx_memblk_addr;	/* start addr of rx mem blocks */
	uint32_t	tx_memblk_addr;	/* start addr of tx mem blocks */
	uint16_t	rx_memblk_num;	/* num of RX mem block */
	uint16_t	tx_memblk_num;	/* num of TX mem block */
} __packed;

#define ACX100_MEMOPT_MEM_INSTR		0x00000000 /* memory access instruct */
#define ACX100_MEMOPT_HOSTDESC		0x00010000 /* host indirect desc */
#define ACX100_MEMOPT_MEMBLOCK		0x00020000 /* local mem block list */
#define ACX100_MEMOPT_IO_INSTR		0x00040000 /* IO instruct */
#define ACX100_MEMOPT_PCICONF		0x00080000 /* PCI conf space */

#define ACX100_MEMBLK_ALIGN		0x20

struct acx100_conf_cca_mode {
	struct acx_conf	confcom;
	uint8_t		cca_mode;
	uint8_t		unknown;
} __packed;

struct acx100_conf_ed_thresh {
	struct acx_conf	confcom;
	uint8_t		ed_thresh;
	uint8_t		unknown[3];
} __packed;

struct acx100_conf_wepkey {
	struct acx_conf	confcom;
	uint8_t		action;	/* see ACX100_WEPKEY_ACT_ */
	uint8_t		key_len;
	uint8_t		key_idx;
#define ACX100_WEPKEY_LEN	29
	uint8_t		key[ACX100_WEPKEY_LEN];
} __packed;

#define ACX100_WEPKEY_ACT_ADD	1

#define ACX100_CONF_FUNC(sg, name)	_ACX_CONF_FUNC(sg, name, 100)
#define ACX_CONF_fw_ring		ACX100_CONF_FW_RING
#define ACX_CONF_memblk_size		ACX_CONF_MEMBLK_SIZE
#define ACX_CONF_mem			ACX100_CONF_MEMOPT
#define ACX_CONF_cca_mode		ACX_CONF_CCA_MODE
#define ACX_CONF_ed_thresh		ACX_CONF_ED_THRESH
#define ACX_CONF_wepkey			ACX_CONF_WEPKEY
ACX100_CONF_FUNC(set, fw_ring);
ACX100_CONF_FUNC(set, memblk_size);
ACX100_CONF_FUNC(set, mem);
ACX100_CONF_FUNC(get, cca_mode);
ACX100_CONF_FUNC(set, cca_mode);
ACX100_CONF_FUNC(get, ed_thresh);
ACX100_CONF_FUNC(set, ed_thresh);
ACX100_CONF_FUNC(set, wepkey);

#define ACXCMD_init_mem			ACXCMD_INIT_MEM
ACX_NOARG_FUNC(init_mem);

static const uint16_t	acx100_reg[ACXREG_MAX] = {
	ACXREG(SOFT_RESET,		0x0000),

	ACXREG(FWMEM_ADDR,		0x0014),
	ACXREG(FWMEM_DATA,		0x0018),
	ACXREG(FWMEM_CTRL,		0x001c),
	ACXREG(FWMEM_START,		0x0020),

	ACXREG(EVENT_MASK,		0x0034),

	ACXREG(INTR_TRIG,		0x007c),
	ACXREG(INTR_MASK,		0x0098),
	ACXREG(INTR_STATUS,		0x00a4),
	ACXREG(INTR_STATUS_CLR,		0x00a8),
	ACXREG(INTR_ACK,		0x00ac),

	ACXREG(HINTR_TRIG,		0x00b0),
	ACXREG(RADIO_ENABLE,		0x0104),

	ACXREG(EEPROM_INIT,		0x02d0),
	ACXREG(EEPROM_CTRL,		0x0250),
	ACXREG(EEPROM_ADDR,		0x0254),
	ACXREG(EEPROM_DATA,		0x0258),
	ACXREG(EEPROM_CONF,		0x025c),
	ACXREG(EEPROM_INFO,		0x02ac),

	ACXREG(PHY_ADDR,		0x0268),
	ACXREG(PHY_DATA,		0x026c),
	ACXREG(PHY_CTRL,		0x0270),

	ACXREG(GPIO_OUT_ENABLE,		0x0290),
	ACXREG(GPIO_OUT,		0x0298),

	ACXREG(CMD_REG_OFFSET,		0x02a4),
	ACXREG(INFO_REG_OFFSET,		0x02a8),

	ACXREG(RESET_SENSE,		0x02d4),
	ACXREG(ECPU_CTRL,		0x02d8) 
};

static const uint8_t	acx100_txpower_maxim[21] = {
	63, 63, 63, 62,
	61, 61, 60, 60,
	59, 58, 57, 55,
	53, 50, 47, 43,
	38, 31, 23, 13,
	0
};

static const uint8_t	acx100_txpower_rfmd[21] = {
	 0,  0,  0,  1,
	 2,  2,  3,  3,
	 4,  5,  6,  8,
	10, 13, 16, 20,
	25, 32, 41, 50,
	63
};

static const uint8_t	acx100_rate_map[45] = {
	[2]	= 0x01,
	[4]	= 0x02,
	[11]	= 0x04,
	[22]	= 0x08,
	[44]	= 0x10
};

static int	acx100_init(struct acx_softc *);
static int	acx100_init_wep(struct acx_softc *);
static int	acx100_init_tmplt(struct acx_softc *);
static int	acx100_init_fw_ring(struct acx_softc *);
static int	acx100_init_memory(struct acx_softc *);

static void	acx100_init_fw_txring(struct acx_softc *, uint32_t);
static void	acx100_init_fw_rxring(struct acx_softc *, uint32_t);

static int	acx100_read_config(struct acx_softc *, struct acx_config *);
static int	acx100_write_config(struct acx_softc *, struct acx_config *);

static void	*acx100_ratectl_attach(struct ieee80211com *, u_int);

static int	acx100_set_txpower(struct acx_softc *);

static uint8_t	acx100_set_fw_txdesc_rate(struct acx_softc *,
					  struct acx_txbuf *,
					  struct ieee80211_node *, int);
static void	acx100_tx_complete(struct acx_softc *, struct acx_txbuf *,
				   int, int);
static void	acx100_set_bss_join_param(struct acx_softc *, void *, int);

static int	acx100_set_wepkey(struct acx_softc *, struct ieee80211_key *,
				  int);

static void	acx100_proc_wep_rxbuf(struct acx_softc *, struct mbuf *, int *);

#define ACX100_CHK_RATE(ifp, rate, rate_idx)	\
	acx100_check_rate(ifp, rate, rate_idx, __func__)

static __inline int
acx100_check_rate(struct ifnet *ifp, u_int rate, int rate_idx,
		  const char *fname)
{
	if (rate >= NELEM(acx100_rate_map)) {
		if_printf(ifp, "%s rate out of range %u (idx %d)\n",
			  fname, rate, rate_idx);
		return -1;
	}

	if (acx100_rate_map[rate] == 0) {
		if_printf(ifp, "%s invalid rate %u (idx %d)\n",
			  fname, rate, rate_idx);
		return -1;
	}
	return 0;
}

void
acx100_set_param(device_t dev)
{
	struct acx_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic = &sc->sc_ic;
	struct acx_firmware *fw = &sc->sc_firmware;

	sc->chip_mem1_rid = PCIR_BAR(1);
	sc->chip_mem2_rid = PCIR_BAR(2);
	sc->chip_ioreg = acx100_reg;
	sc->chip_hw_crypt = 1;
	sc->chip_intr_enable = ACX100_INTR_ENABLE;
	sc->chip_intr_disable = ACX100_INTR_DISABLE;
	sc->chip_gpio_pled = ACX100_GPIO_POWER_LED;
	sc->chip_ee_eaddr_ofs = ACX100_EE_EADDR_OFS;
	sc->chip_txdesc1_len = ACX_FRAME_HDRLEN;
	sc->chip_fw_txdesc_ctrl = DESC_CTRL_AUTODMA |
				  DESC_CTRL_RECLAIM |
				  DESC_CTRL_FIRST_FRAG;
	sc->chip_short_retry_limit = 7;
	sc->chip_rssi_corr = ACX100_RSSI_CORR;

	sc->chip_phymode = IEEE80211_MODE_11B;
	sc->chip_chan_flags = IEEE80211_CHAN_B;

	ic->ic_phytype = IEEE80211_T_DS;
	if (acx_enable_pbcc)
		ic->ic_sup_rates[IEEE80211_MODE_11B] = acx_rates_11b_pbcc;
	else
		ic->ic_sup_rates[IEEE80211_MODE_11B] = acx_rates_11b;

	IEEE80211_ONOE_PARAM_SETUP(&sc->sc_onoe_param);

	ic->ic_ratectl.rc_st_ratectl_cap = IEEE80211_RATECTL_CAP_ONOE;
	ic->ic_ratectl.rc_st_ratectl = IEEE80211_RATECTL_ONOE;
	ic->ic_ratectl.rc_st_attach = acx100_ratectl_attach;

	sc->chip_init = acx100_init;
	sc->chip_set_wepkey = acx100_set_wepkey;
	sc->chip_read_config = acx100_read_config;
	sc->chip_write_config = acx100_write_config;
	sc->chip_set_fw_txdesc_rate = acx100_set_fw_txdesc_rate;
	sc->chip_tx_complete = acx100_tx_complete;
	sc->chip_set_bss_join_param = acx100_set_bss_join_param;
	sc->chip_proc_wep_rxbuf = acx100_proc_wep_rxbuf;

	fw->combined_radio_fw = 0;
	fw->fwdir = "100";
}

static int
acx100_init(struct acx_softc *sc)
{
	/*
	 * NOTE:
	 * Order of initialization:
	 * 1) WEP
	 * 2) Templates
	 * 3) Firmware TX/RX ring
	 * 4) Hardware memory
	 * Above order is critical to get a correct memory map
	 */

	if (acx100_init_wep(sc) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't initialize wep\n",
			  __func__);
		return ENXIO;
	}

	if (acx100_init_tmplt(sc) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't initialize templates\n",
			  __func__);
		return ENXIO;
	}

	if (acx100_init_fw_ring(sc) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't initialize fw ring\n",
			  __func__);
		return ENXIO;
	}

	if (acx100_init_memory(sc) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't initialize hw memory\n",
			  __func__);
		return ENXIO;
	}
	return 0;
}

static int
acx100_init_wep(struct acx_softc *sc)
{
	struct acx_conf_wepopt wep_opt;
	struct acx_conf_mmap mem_map;

	/* Set WEP cache start/end address */
	if (acx_get_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't get mmap\n");
		return 1;
	}

	mem_map.wep_cache_start = htole32(le32toh(mem_map.code_end) + 4);
	mem_map.wep_cache_end = htole32(le32toh(mem_map.code_end) + 4);
	if (acx_set_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set mmap\n");
		return 1;
	}

	/* Set WEP options */
	wep_opt.nkey = htole16(IEEE80211_WEP_NKID + 10);
	wep_opt.opt = WEPOPT_HDWEP;
	if (acx_set_wepopt_conf(sc, &wep_opt) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set wep opt\n");
		return 1;
	}
	return 0;
}

static int
acx100_init_tmplt(struct acx_softc *sc)
{
	struct acx_conf_mmap mem_map;

	/* Set templates start address */
	if (acx_get_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't get mmap\n");
		return 1;
	}

	mem_map.pkt_tmplt_start = mem_map.wep_cache_end;
	if (acx_set_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set mmap\n");
		return 1;
	}

	/* Initialize various packet templates */
	if (acx_init_tmplt_ordered(sc) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't init tmplt\n");
		return 1;
	}
	return 0;
}

static int
acx100_init_fw_ring(struct acx_softc *sc)
{
	struct acx100_conf_fw_ring ring;
	struct acx_conf_mmap mem_map;
	uint32_t txring_start, rxring_start, ring_end;

	/* Set firmware descriptor ring start address */
	if (acx_get_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't get mmap\n");
		return 1;
	}

	txring_start = le32toh(mem_map.pkt_tmplt_end) + 4;
	rxring_start = txring_start + ACX100_FW_TXRING_SIZE;
	ring_end = rxring_start + ACX100_FW_RXRING_SIZE;

	mem_map.fw_desc_start = htole32(txring_start);
	if (acx_set_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set mmap\n");
		return 1;
	}

	/* Set firmware descriptor ring configure */
	bzero(&ring, sizeof(ring));
	ring.fw_ring_size = htole32(ACX100_FW_TXRING_SIZE +
				    ACX100_FW_RXRING_SIZE + 8);

	ring.fw_txring_num = 1;
	ring.fw_txring_addr = htole32(txring_start);
	ring.fw_txring_prio = ACX100_TXRING_PRIO_DEFAULT;
	ring.fw_txdesc_num = 0; /* XXX ignored?? */

	ring.fw_rxring_addr = htole32(rxring_start);
	ring.fw_rxdesc_num = 0; /* XXX ignored?? */

	ring.opt = ACX100_RINGOPT_AUTO_RESET;
	ACX100_SET_RING_END(&ring, ring_end);
	if (acx100_set_fw_ring_conf(sc, &ring) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set fw ring configure\n");
		return 1;
	}

	/* Setup firmware TX/RX descriptor ring */
	acx100_init_fw_txring(sc, txring_start);
	acx100_init_fw_rxring(sc, rxring_start);

	return 0;
}

#define MEMBLK_ALIGN(addr)	\
	(((addr) + (ACX100_MEMBLK_ALIGN - 1)) &	~(ACX100_MEMBLK_ALIGN - 1))

static int
acx100_init_memory(struct acx_softc *sc)
{
	struct acx100_conf_memblk_size memblk_sz;
	struct acx100_conf_mem mem;
	struct acx_conf_mmap mem_map;
	uint32_t memblk_start, memblk_end;
	int total_memblk, txblk_num, rxblk_num;

	/* Set memory block start address */
	if (acx_get_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't get mmap\n");
		return 1;
	}

	mem_map.memblk_start =
		htole32(MEMBLK_ALIGN(le32toh(mem_map.fw_desc_end) + 4));

	if (acx_set_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set mmap\n");
		return 1;
	}

	/* Set memory block size */
	memblk_sz.memblk_size = htole16(ACX_MEMBLOCK_SIZE);
	if (acx100_set_memblk_size_conf(sc, &memblk_sz) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set mem block size\n");
		return 1;
	}

	/* Get memory map after setting it */
	if (acx_get_mmap_conf(sc, &mem_map) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't get mmap again\n");
		return 1;
	}
	memblk_start = le32toh(mem_map.memblk_start);
	memblk_end = le32toh(mem_map.memblk_end);

	/* Set memory options */
	mem.opt = htole32(ACX100_MEMOPT_MEMBLOCK | ACX100_MEMOPT_HOSTDESC);
	mem.h_rxring_paddr = htole32(sc->sc_ring_data.rx_ring_paddr);

	total_memblk = (memblk_end - memblk_start) / ACX_MEMBLOCK_SIZE;

	rxblk_num = total_memblk / 2;		/* 50% */
	txblk_num = total_memblk - rxblk_num;	/* 50% */

	DPRINTF((&sc->sc_ic.ic_if, "\ttotal memory blocks\t%d\n"
				   "\trx memory blocks\t%d\n"
				   "\ttx memory blocks\t%d\n",
				   total_memblk, rxblk_num, txblk_num));

	mem.rx_memblk_num = htole16(rxblk_num);
	mem.tx_memblk_num = htole16(txblk_num);

	mem.rx_memblk_addr = htole32(MEMBLK_ALIGN(memblk_start));
	mem.tx_memblk_addr =
		htole32(MEMBLK_ALIGN(memblk_start +
				     (ACX_MEMBLOCK_SIZE * rxblk_num)));

	if (acx100_set_mem_conf(sc, &mem) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't set mem options\n");
		return 1;
	}

	/* Initialize memory */
	if (acx_init_mem(sc) != 0) {
		if_printf(&sc->sc_ic.ic_if, "can't init mem\n");
		return 1;
	}
	return 0;
}

#undef MEMBLK_ALIGN

static void
acx100_init_fw_txring(struct acx_softc *sc, uint32_t fw_txdesc_start)
{
	struct acx_fw_txdesc fw_desc;
	struct acx_txbuf *tx_buf;
	uint32_t desc_paddr, fw_desc_offset;
	int i;

	bzero(&fw_desc, sizeof(fw_desc));
	fw_desc.f_tx_ctrl = DESC_CTRL_HOSTOWN |
			    DESC_CTRL_RECLAIM |
			    DESC_CTRL_AUTODMA |
			    DESC_CTRL_FIRST_FRAG;

	tx_buf = sc->sc_buf_data.tx_buf;
	fw_desc_offset = fw_txdesc_start;
	desc_paddr = sc->sc_ring_data.tx_ring_paddr;

	for (i = 0; i < ACX_TX_DESC_CNT; ++i) {
		fw_desc.f_tx_host_desc = htole32(desc_paddr);

		if (i == ACX_TX_DESC_CNT - 1) {
			fw_desc.f_tx_next_desc = htole32(fw_txdesc_start);
		} else {
			fw_desc.f_tx_next_desc =
				htole32(fw_desc_offset +
					sizeof(struct acx_fw_txdesc));
		}

		tx_buf[i].tb_fwdesc_ofs = fw_desc_offset;
		DESC_WRITE_REGION_1(sc, fw_desc_offset, &fw_desc,
				    sizeof(fw_desc));

		desc_paddr += (2 * sizeof(struct acx_host_desc));
		fw_desc_offset += sizeof(fw_desc);
	}
}

static void
acx100_init_fw_rxring(struct acx_softc *sc, uint32_t fw_rxdesc_start)
{
	struct acx_fw_rxdesc fw_desc;
	uint32_t fw_desc_offset;
	int i;

	bzero(&fw_desc, sizeof(fw_desc));
	fw_desc.f_rx_ctrl = DESC_CTRL_RECLAIM | DESC_CTRL_AUTODMA;

	fw_desc_offset = fw_rxdesc_start;

	for (i = 0; i < ACX_RX_DESC_CNT; ++i) {
		if (i == ACX_RX_DESC_CNT - 1) {
			fw_desc.f_rx_next_desc = htole32(fw_rxdesc_start);
		} else {
			fw_desc.f_rx_next_desc =
				htole32(fw_desc_offset +
					sizeof(struct acx_fw_rxdesc));
		}

		DESC_WRITE_REGION_1(sc, fw_desc_offset, &fw_desc,
				    sizeof(fw_desc));

		fw_desc_offset += sizeof(fw_desc);
	}
}

static int
acx100_read_config(struct acx_softc *sc, struct acx_config *conf)
{
	struct acx100_conf_cca_mode cca;
	struct acx100_conf_ed_thresh ed;

	/*
	 * NOTE:
	 * CCA mode and ED threshold MUST be read during initialization
	 * or the acx100 card won't work as expected
	 */

	/* Get CCA mode */
	if (acx100_get_cca_mode_conf(sc, &cca) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't get cca mode\n",
			  __func__);
		return ENXIO;
	}
	conf->cca_mode = cca.cca_mode;
	DPRINTF((&sc->sc_ic.ic_if, "cca mode %02x\n", cca.cca_mode));

	/* Get ED threshold */
	if (acx100_get_ed_thresh_conf(sc, &ed) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't get ed threshold\n",
			  __func__);
		return ENXIO;
	}
	conf->ed_thresh = ed.ed_thresh;
	DPRINTF((&sc->sc_ic.ic_if, "ed threshold %02x\n", ed.ed_thresh));

	return 0;
}

static int
acx100_write_config(struct acx_softc *sc, struct acx_config *conf)
{
	struct acx100_conf_cca_mode cca;
	struct acx100_conf_ed_thresh ed;

	/* Set CCA mode */
	cca.cca_mode = conf->cca_mode;
	if (acx100_set_cca_mode_conf(sc, &cca) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't set cca mode\n",
			  __func__);
		return ENXIO;
	}

	/* Set ED threshold */
	ed.ed_thresh = conf->ed_thresh;
	if (acx100_set_ed_thresh_conf(sc, &ed) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s can't set ed threshold\n",
			  __func__);
		return ENXIO;
	}

	/* Set TX power */
	acx100_set_txpower(sc);	/* ignore return value */

	return 0;
}

static int
acx100_set_txpower(struct acx_softc *sc)
{
	const uint8_t *map;

	switch (sc->sc_radio_type) {
	case ACX_RADIO_TYPE_MAXIM:
		map = acx100_txpower_maxim;
		break;
	case ACX_RADIO_TYPE_RFMD:
	case ACX_RADIO_TYPE_RALINK:
		map = acx100_txpower_rfmd;
		break;
	default:
		if_printf(&sc->sc_ic.ic_if, "TX power for radio type 0x%02x "
			  "can't be set yet\n", sc->sc_radio_type);
		return 1;
	}

	acx_write_phyreg(sc, ACXRV_PHYREG_TXPOWER, map[ACX100_TXPOWER]);
	return 0;
}

static uint8_t
acx100_set_fw_txdesc_rate(struct acx_softc *sc, struct acx_txbuf *tx_buf,
			  struct ieee80211_node *ni, int data_len)
{
	int rate;

	tx_buf->tb_rateidx_len = 1;
	if (ni == NULL) {
		rate = 2;	/* 1Mbit/s */
		tx_buf->tb_rateidx[0] = 0;
	} else {
		ieee80211_ratectl_findrate(ni, data_len,
					   tx_buf->tb_rateidx, 1);
		rate = IEEE80211_RS_RATE(&ni->ni_rates,
					 tx_buf->tb_rateidx[0]);
		if (ACX100_CHK_RATE(&sc->sc_ic.ic_if, rate,
				    tx_buf->tb_rateidx[0]) < 0)
			rate = 2;
	}
	FW_TXDESC_SETFIELD_1(sc, tx_buf, f_tx_rate100, ACX100_RATE(rate));

	return rate;
}

static void
acx100_set_bss_join_param(struct acx_softc *sc, void *param, int dtim_intvl)
{
	struct acx100_bss_join *bj = param;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	const struct ieee80211_rateset *rs = &sc->sc_ic.ic_bss->ni_rates;
	int i;

	bj->basic_rates = 0;
	bj->op_rates = 0;
	for (i = 0; i < rs->rs_nrates; ++i) {
		u_int map_idx = IEEE80211_RS_RATE(rs, i);
		uint8_t rate;

		if (ACX100_CHK_RATE(ifp, map_idx, i) < 0)
			continue;

		rate = acx100_rate_map[map_idx];
		if (rs->rs_rates[i] & IEEE80211_RATE_BASIC)
			bj->basic_rates |= rate;
		bj->op_rates |= rate;
	}
	DPRINTF((ifp, "basic rates:0x%02x, op rates:0x%02x\n",
		 bj->basic_rates, bj->op_rates));

	bj->dtim_intvl = dtim_intvl;
}

static int
acx100_set_wepkey(struct acx_softc *sc, struct ieee80211_key *wk, int wk_idx)
{
	struct acx100_conf_wepkey conf_wk;

	if (wk->wk_keylen > ACX100_WEPKEY_LEN) {
		if_printf(&sc->sc_ic.ic_if, "%dth WEP key size beyond %d\n",
			  wk_idx, ACX100_WEPKEY_LEN);
		return EINVAL;
	}

	conf_wk.action = ACX100_WEPKEY_ACT_ADD;
	conf_wk.key_len = wk->wk_keylen;
	conf_wk.key_idx = wk_idx;
	bcopy(wk->wk_key, conf_wk.key, wk->wk_keylen);
	if (acx100_set_wepkey_conf(sc, &conf_wk) != 0) {
		if_printf(&sc->sc_ic.ic_if, "%s set %dth WEP key failed\n",
			  __func__, wk_idx);
		return ENXIO;
	}
	return 0;
}

static void
acx100_proc_wep_rxbuf(struct acx_softc *sc, struct mbuf *m, int *len)
{
	int mac_hdrlen;
	struct ieee80211_frame *f;

	/*
	 * Strip leading IV and KID, and trailing CRC
	 */

	f = mtod(m, struct ieee80211_frame *);

	if ((f->i_fc[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_DSTODS)
		mac_hdrlen = sizeof(struct ieee80211_frame_addr4);
	else
		mac_hdrlen = sizeof(struct ieee80211_frame);

#define IEEEWEP_IVLEN	(IEEE80211_WEP_IVLEN + IEEE80211_WEP_KIDLEN)
#define IEEEWEP_EXLEN	(IEEEWEP_IVLEN + IEEE80211_WEP_CRCLEN)

	*len = *len - IEEEWEP_EXLEN;

	/* Move MAC header toward frame body */
	ovbcopy(f, (uint8_t *)f + IEEEWEP_IVLEN, mac_hdrlen);
	m_adj(m, IEEEWEP_IVLEN);

#undef IEEEWEP_EXLEN
#undef IEEEWEP_IVLEN
}

static void
acx100_tx_complete(struct acx_softc *sc, struct acx_txbuf *tx_buf,
		   int frame_len, int is_fail)
{
	int rts_retries, data_retries;
	struct ieee80211_ratectl_res rc_res;

	rts_retries = FW_TXDESC_GETFIELD_1(sc, tx_buf, f_tx_rts_nretry);
	data_retries = FW_TXDESC_GETFIELD_1(sc, tx_buf, f_tx_data_nretry);

	rc_res.rc_res_rateidx = tx_buf->tb_rateidx[0];
	rc_res.rc_res_tries = data_retries + 1;

	ieee80211_ratectl_tx_complete(tx_buf->tb_node, frame_len,
				      &rc_res, 1, data_retries, rts_retries,
				      is_fail);
}

static void *
acx100_ratectl_attach(struct ieee80211com *ic, u_int rc)
{
	struct acx_softc *sc = ic->ic_if.if_softc;

	switch (rc) {
	case IEEE80211_RATECTL_ONOE:
		return &sc->sc_onoe_param;
	case IEEE80211_RATECTL_NONE:
		/* This could only happen during detaching */
		return NULL;
	default:
		panic("unknown rate control algo %u\n", rc);
		return NULL;
	}
}
