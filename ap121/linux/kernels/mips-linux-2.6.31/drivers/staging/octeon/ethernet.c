/**********************************************************************
 * Author: Cavium Networks
 *
 * Contact: support@caviumnetworks.com
 * This file is part of the OCTEON SDK
 *
 * Copyright (c) 2003-2007 Cavium Networks
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, Version 2, as
 * published by the Free Software Foundation.
 *
 * This file is distributed in the hope that it will be useful, but
 * AS-IS and WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE, TITLE, or
 * NONINFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this file; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 * or visit http://www.gnu.org/licenses/.
 *
 * This file may also be available under a different license from Cavium.
 * Contact Cavium Networks for more information
**********************************************************************/
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/mii.h>

#include <net/dst.h>

#include <asm/octeon/octeon.h>

#include "ethernet-defines.h"
#include "octeon-ethernet.h"
#include "ethernet-mem.h"
#include "ethernet-rx.h"
#include "ethernet-tx.h"
#include "ethernet-mdio.h"
#include "ethernet-util.h"
#include "ethernet-proc.h"


#include "cvmx-pip.h"
#include "cvmx-pko.h"
#include "cvmx-fau.h"
#include "cvmx-ipd.h"
#include "cvmx-helper.h"

#include "cvmx-gmxx-defs.h"
#include "cvmx-smix-defs.h"

#if defined(CONFIG_CAVIUM_OCTEON_NUM_PACKET_BUFFERS) \
	&& CONFIG_CAVIUM_OCTEON_NUM_PACKET_BUFFERS
int num_packet_buffers = CONFIG_CAVIUM_OCTEON_NUM_PACKET_BUFFERS;
#else
int num_packet_buffers = 1024;
#endif
module_param(num_packet_buffers, int, 0444);
MODULE_PARM_DESC(num_packet_buffers, "\n"
	"\tNumber of packet buffers to allocate and store in the\n"
	"\tFPA. By default, 1024 packet buffers are used unless\n"
	"\tCONFIG_CAVIUM_OCTEON_NUM_PACKET_BUFFERS is defined.");

int pow_receive_group = 15;
module_param(pow_receive_group, int, 0444);
MODULE_PARM_DESC(pow_receive_group, "\n"
	"\tPOW group to receive packets from. All ethernet hardware\n"
	"\twill be configured to send incomming packets to this POW\n"
	"\tgroup. Also any other software can submit packets to this\n"
	"\tgroup for the kernel to process.");

int pow_send_group = -1;
module_param(pow_send_group, int, 0644);
MODULE_PARM_DESC(pow_send_group, "\n"
	"\tPOW group to send packets to other software on. This\n"
	"\tcontrols the creation of the virtual device pow0.\n"
	"\talways_use_pow also depends on this value.");

int always_use_pow;
module_param(always_use_pow, int, 0444);
MODULE_PARM_DESC(always_use_pow, "\n"
	"\tWhen set, always send to the pow group. This will cause\n"
	"\tpackets sent to real ethernet devices to be sent to the\n"
	"\tPOW group instead of the hardware. Unless some other\n"
	"\tapplication changes the config, packets will still be\n"
	"\treceived from the low level hardware. Use this option\n"
	"\tto allow a CVMX app to intercept all packets from the\n"
	"\tlinux kernel. You must specify pow_send_group along with\n"
	"\tthis option.");

char pow_send_list[128] = "";
module_param_string(pow_send_list, pow_send_list, sizeof(pow_send_list), 0444);
MODULE_PARM_DESC(pow_send_list, "\n"
	"\tComma separated list of ethernet devices that should use the\n"
	"\tPOW for transmit instead of the actual ethernet hardware. This\n"
	"\tis a per port version of always_use_pow. always_use_pow takes\n"
	"\tprecedence over this list. For example, setting this to\n"
	"\t\"eth2,spi3,spi7\" would cause these three devices to transmit\n"
	"\tusing the pow_send_group.");

static int disable_core_queueing = 1;
module_param(disable_core_queueing, int, 0444);
MODULE_PARM_DESC(disable_core_queueing, "\n"
	"\tWhen set the networking core's tx_queue_len is set to zero.  This\n"
	"\tallows packets to be sent without lock contention in the packet\n"
	"\tscheduler resulting in some cases in improved throughput.\n");

/**
 * Periodic timer to check auto negotiation
 */
static struct timer_list cvm_oct_poll_timer;

/**
 * Array of every ethernet device owned by this driver indexed by
 * the ipd input port number.
 */
struct net_device *cvm_oct_device[TOTAL_NUMBER_OF_PORTS];

extern struct semaphore mdio_sem;

/**
 * Periodic timer tick for slow management operations
 *
 * @arg:    Device to check
 */
static void cvm_do_timer(unsigned long arg)
{
	int32_t skb_to_free, undo;
	int queues_per_port;
	int qos;
	struct octeon_ethernet *priv;
	static int port;

	if (port >= CVMX_PIP_NUM_INPUT_PORTS) {
		/*
		 * All ports have been polled. Start the next
		 * iteration through the ports in one second.
		 */
		port = 0;
		mod_timer(&cvm_oct_poll_timer, jiffies + HZ);
		return;
	}
	if (!cvm_oct_device[port])
		goto out;

	priv = netdev_priv(cvm_oct_device[port]);
	if (priv->poll) {
		/* skip polling if we don't get the lock */
		if (!down_trylock(&mdio_sem)) {
			priv->poll(cvm_oct_device[port]);
			up(&mdio_sem);
		}
	}

	queues_per_port = cvmx_pko_get_num_queues(port);
	/* Drain any pending packets in the free list */
	for (qos = 0; qos < queues_per_port; qos++) {
		if (skb_queue_len(&priv->tx_free_list[qos]) == 0)
			continue;
		skb_to_free = cvmx_fau_fetch_and_add32(priv->fau + qos * 4,
						       MAX_SKB_TO_FREE);
		undo = skb_to_free > 0 ?
			MAX_SKB_TO_FREE : skb_to_free + MAX_SKB_TO_FREE;
		if (undo > 0)
			cvmx_fau_atomic_add32(priv->fau+qos*4, -undo);
		skb_to_free = -skb_to_free > MAX_SKB_TO_FREE ?
			MAX_SKB_TO_FREE : -skb_to_free;
		cvm_oct_free_tx_skbs(priv, skb_to_free, qos, 1);
	}
	cvm_oct_device[port]->netdev_ops->ndo_get_stats(cvm_oct_device[port]);

out:
	port++;
	/* Poll the next port in a 50th of a second.
	   This spreads the polling of ports out a little bit */
	mod_timer(&cvm_oct_poll_timer, jiffies + HZ / 50);
}

/**
 * Configure common hardware for all interfaces
 */
static __init void cvm_oct_configure_common_hw(void)
{
	int r;
	/* Setup the FPA */
	cvmx_fpa_enable();
	cvm_oct_mem_fill_fpa(CVMX_FPA_PACKET_POOL, CVMX_FPA_PACKET_POOL_SIZE,
			     num_packet_buffers);
	cvm_oct_mem_fill_fpa(CVMX_FPA_WQE_POOL, CVMX_FPA_WQE_POOL_SIZE,
			     num_packet_buffers);
	if (CVMX_FPA_OUTPUT_BUFFER_POOL != CVMX_FPA_PACKET_POOL)
		cvm_oct_mem_fill_fpa(CVMX_FPA_OUTPUT_BUFFER_POOL,
				     CVMX_FPA_OUTPUT_BUFFER_POOL_SIZE, 128);

	if (USE_RED)
		cvmx_helper_setup_red(num_packet_buffers / 4,
				      num_packet_buffers / 8);

	/* Enable the MII interface */
	if (!octeon_is_simulation())
		cvmx_write_csr(CVMX_SMIX_EN(0), 1);

	/* Register an IRQ hander for to receive POW interrupts */
	r = request_irq(OCTEON_IRQ_WORKQ0 + pow_receive_group,
			cvm_oct_do_interrupt, IRQF_SHARED, "Ethernet",
			cvm_oct_device);

#if defined(CONFIG_SMP) && 0
	if (USE_MULTICORE_RECEIVE) {
		irq_set_affinity(OCTEON_IRQ_WORKQ0 + pow_receive_group,
				 cpu_online_mask);
	}
#endif
}

/**
 * Free a work queue entry received in a intercept callback.
 *
 * @work_queue_entry:
 *               Work queue entry to free
 * Returns Zero on success, Negative on failure.
 */
int cvm_oct_free_work(void *work_queue_entry)
{
	cvmx_wqe_t *work = work_queue_entry;

	int segments = work->word2.s.bufs;
	union cvmx_buf_ptr segment_ptr = work->packet_ptr;

	while (segments--) {
		union cvmx_buf_ptr next_ptr = *(union cvmx_buf_ptr *)
			cvmx_phys_to_ptr(segment_ptr.s.addr - 8);
		if (unlikely(!segment_ptr.s.i))
			cvmx_fpa_free(cvm_oct_get_buffer_ptr(segment_ptr),
				      segment_ptr.s.pool,
				      DONT_WRITEBACK(CVMX_FPA_PACKET_POOL_SIZE /
						     128));
		segment_ptr = next_ptr;
	}
	cvmx_fpa_free(work, CVMX_FPA_WQE_POOL, DONT_WRITEBACK(1));

	return 0;
}
EXPORT_SYMBOL(cvm_oct_free_work);

/**
 * Get the low level ethernet statistics
 *
 * @dev:    Device to get the statistics from
 * Returns Pointer to the statistics
 */
static struct net_device_stats *cvm_oct_common_get_stats(struct net_device *dev)
{
	cvmx_pip_port_status_t rx_status;
	cvmx_pko_port_status_t tx_status;
	struct octeon_ethernet *priv = netdev_priv(dev);

	if (priv->port < CVMX_PIP_NUM_INPUT_PORTS) {
		if (octeon_is_simulation()) {
			/* The simulator doesn't support statistics */
			memset(&rx_status, 0, sizeof(rx_status));
			memset(&tx_status, 0, sizeof(tx_status));
		} else {
			cvmx_pip_get_port_status(priv->port, 1, &rx_status);
			cvmx_pko_get_port_status(priv->port, 1, &tx_status);
		}

		priv->stats.rx_packets += rx_status.inb_packets;
		priv->stats.tx_packets += tx_status.packets;
		priv->stats.rx_bytes += rx_status.inb_octets;
		priv->stats.tx_bytes += tx_status.octets;
		priv->stats.multicast += rx_status.multicast_packets;
		priv->stats.rx_crc_errors += rx_status.inb_errors;
		priv->stats.rx_frame_errors += rx_status.fcs_align_err_packets;

		/*
		 * The drop counter must be incremented atomically
		 * since the RX tasklet also increments it.
		 */
#ifdef CONFIG_64BIT
		atomic64_add(rx_status.dropped_packets,
			     (atomic64_t *)&priv->stats.rx_dropped);
#else
		atomic_add(rx_status.dropped_packets,
			     (atomic_t *)&priv->stats.rx_dropped);
#endif
	}

	return &priv->stats;
}

/**
 * Change the link MTU. Unimplemented
 *
 * @dev:     Device to change
 * @new_mtu: The new MTU
 *
 * Returns Zero on success
 */
static int cvm_oct_common_change_mtu(struct net_device *dev, int new_mtu)
{
	struct octeon_ethernet *priv = netdev_priv(dev);
	int interface = INTERFACE(priv->port);
	int index = INDEX(priv->port);
#if defined(CONFIG_VLAN_8021Q) || defined(CONFIG_VLAN_8021Q_MODULE)
	int vlan_bytes = 4;
#else
	int vlan_bytes = 0;
#endif

	/*
	 * Limit the MTU to make sure the ethernet packets are between
	 * 64 bytes and 65535 bytes.
	 */
	if ((new_mtu + 14 + 4 + vlan_bytes < 64)
	    || (new_mtu + 14 + 4 + vlan_bytes > 65392)) {
		pr_err("MTU must be between %d and %d.\n",
		       64 - 14 - 4 - vlan_bytes, 65392 - 14 - 4 - vlan_bytes);
		return -EINVAL;
	}
	dev->mtu = new_mtu;

	if ((interface < 2)
	    && (cvmx_helper_interface_get_mode(interface) !=
		CVMX_HELPER_INTERFACE_MODE_SPI)) {
		/* Add ethernet header and FCS, and VLAN if configured. */
		int max_packet = new_mtu + 14 + 4 + vlan_bytes;

		if (OCTEON_IS_MODEL(OCTEON_CN3XXX)
		    || OCTEON_IS_MODEL(OCTEON_CN58XX)) {
			/* Signal errors on packets larger than the MTU */
			cvmx_write_csr(CVMX_GMXX_RXX_FRM_MAX(index, interface),
				       max_packet);
		} else {
			/*
			 * Set the hardware to truncate packets larger
			 * than the MTU and smaller the 64 bytes.
			 */
			union cvmx_pip_frm_len_chkx frm_len_chk;
			frm_len_chk.u64 = 0;
			frm_len_chk.s.minlen = 64;
			frm_len_chk.s.maxlen = max_packet;
			cvmx_write_csr(CVMX_PIP_FRM_LEN_CHKX(interface),
				       frm_len_chk.u64);
		}
		/*
		 * Set the hardware to truncate packets larger than
		 * the MTU. The jabber register must be set to a
		 * multiple of 8 bytes, so round up.
		 */
		cvmx_write_csr(CVMX_GMXX_RXX_JABBER(index, interface),
			       (max_packet + 7) & ~7u);
	}
	return 0;
}

/**
 * Set the multicast list. Currently unimplemented.
 *
 * @dev:    Device to work on
 */
static void cvm_oct_common_set_multicast_list(struct net_device *dev)
{
	union cvmx_gmxx_prtx_cfg gmx_cfg;
	struct octeon_ethernet *priv = netdev_priv(dev);
	int interface = INTERFACE(priv->port);
	int index = INDEX(priv->port);

	if ((interface < 2)
	    && (cvmx_helper_interface_get_mode(interface) !=
		CVMX_HELPER_INTERFACE_MODE_SPI)) {
		union cvmx_gmxx_rxx_adr_ctl control;
		control.u64 = 0;
		control.s.bcst = 1;	/* Allow broadcast MAC addresses */

		if (dev->mc_list || (dev->flags & IFF_ALLMULTI) ||
		    (dev->flags & IFF_PROMISC))
			/* Force accept multicast packets */
			control.s.mcst = 2;
		else
			/* Force reject multicat packets */
			control.s.mcst = 1;

		if (dev->flags & IFF_PROMISC)
			/*
			 * Reject matches if promisc. Since CAM is
			 * shut off, should accept everything.
			 */
			control.s.cam_mode = 0;
		else
			/* Filter packets based on the CAM */
			control.s.cam_mode = 1;

		gmx_cfg.u64 =
		    cvmx_read_csr(CVMX_GMXX_PRTX_CFG(index, interface));
		cvmx_write_csr(CVMX_GMXX_PRTX_CFG(index, interface),
			       gmx_cfg.u64 & ~1ull);

		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CTL(index, interface),
			       control.u64);
		if (dev->flags & IFF_PROMISC)
			cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM_EN
				       (index, interface), 0);
		else
			cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM_EN
				       (index, interface), 1);

		cvmx_write_csr(CVMX_GMXX_PRTX_CFG(index, interface),
			       gmx_cfg.u64);
	}
}

/**
 * Set the hardware MAC address for a device
 *
 * @dev:    Device to change the MAC address for
 * @addr:   Address structure to change it too. MAC address is addr + 2.
 * Returns Zero on success
 */
static int cvm_oct_common_set_mac_address(struct net_device *dev, void *addr)
{
	struct octeon_ethernet *priv = netdev_priv(dev);
	union cvmx_gmxx_prtx_cfg gmx_cfg;
	int interface = INTERFACE(priv->port);
	int index = INDEX(priv->port);

	memcpy(dev->dev_addr, addr + 2, 6);

	if ((interface < 2)
	    && (cvmx_helper_interface_get_mode(interface) !=
		CVMX_HELPER_INTERFACE_MODE_SPI)) {
		int i;
		uint8_t *ptr = addr;
		uint64_t mac = 0;
		for (i = 0; i < 6; i++)
			mac = (mac << 8) | (uint64_t) (ptr[i + 2]);

		gmx_cfg.u64 =
		    cvmx_read_csr(CVMX_GMXX_PRTX_CFG(index, interface));
		cvmx_write_csr(CVMX_GMXX_PRTX_CFG(index, interface),
			       gmx_cfg.u64 & ~1ull);

		cvmx_write_csr(CVMX_GMXX_SMACX(index, interface), mac);
		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM0(index, interface),
			       ptr[2]);
		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM1(index, interface),
			       ptr[3]);
		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM2(index, interface),
			       ptr[4]);
		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM3(index, interface),
			       ptr[5]);
		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM4(index, interface),
			       ptr[6]);
		cvmx_write_csr(CVMX_GMXX_RXX_ADR_CAM5(index, interface),
			       ptr[7]);
		cvm_oct_common_set_multicast_list(dev);
		cvmx_write_csr(CVMX_GMXX_PRTX_CFG(index, interface),
			       gmx_cfg.u64);
	}
	return 0;
}

/**
 * Per network device initialization
 *
 * @dev:    Device to initialize
 * Returns Zero on success
 */
int cvm_oct_common_init(struct net_device *dev)
{
	static int count;
	char mac[8] = { 0x00, 0x00,
		octeon_bootinfo->mac_addr_base[0],
		octeon_bootinfo->mac_addr_base[1],
		octeon_bootinfo->mac_addr_base[2],
		octeon_bootinfo->mac_addr_base[3],
		octeon_bootinfo->mac_addr_base[4],
		octeon_bootinfo->mac_addr_base[5] + count
	};
	struct octeon_ethernet *priv = netdev_priv(dev);

	/*
	 * Force the interface to use the POW send if always_use_pow
	 * was specified or it is in the pow send list.
	 */
	if ((pow_send_group != -1)
	    && (always_use_pow || strstr(pow_send_list, dev->name)))
		priv->queue = -1;

	if (priv->queue != -1 && USE_HW_TCPUDP_CHECKSUM)
		dev->features |= NETIF_F_IP_CSUM;

	count++;

	/* We do our own locking, Linux doesn't need to */
	dev->features |= NETIF_F_LLTX;
	SET_ETHTOOL_OPS(dev, &cvm_oct_ethtool_ops);

	cvm_oct_mdio_setup_device(dev);
	dev->netdev_ops->ndo_set_mac_address(dev, mac);
	dev->netdev_ops->ndo_change_mtu(dev, dev->mtu);

	/*
	 * Zero out stats for port so we won't mistakenly show
	 * counters from the bootloader.
	 */
	memset(dev->netdev_ops->ndo_get_stats(dev), 0,
	       sizeof(struct net_device_stats));

	return 0;
}

void cvm_oct_common_uninit(struct net_device *dev)
{
	/* Currently nothing to do */
}

static const struct net_device_ops cvm_oct_npi_netdev_ops = {
	.ndo_init		= cvm_oct_common_init,
	.ndo_uninit		= cvm_oct_common_uninit,
	.ndo_start_xmit		= cvm_oct_xmit,
	.ndo_set_multicast_list	= cvm_oct_common_set_multicast_list,
	.ndo_set_mac_address	= cvm_oct_common_set_mac_address,
	.ndo_do_ioctl		= cvm_oct_ioctl,
	.ndo_change_mtu		= cvm_oct_common_change_mtu,
	.ndo_get_stats		= cvm_oct_common_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= cvm_oct_poll_controller,
#endif
};
static const struct net_device_ops cvm_oct_xaui_netdev_ops = {
	.ndo_init		= cvm_oct_xaui_init,
	.ndo_uninit		= cvm_oct_xaui_uninit,
	.ndo_open		= cvm_oct_xaui_open,
	.ndo_stop		= cvm_oct_xaui_stop,
	.ndo_start_xmit		= cvm_oct_xmit,
	.ndo_set_multicast_list	= cvm_oct_common_set_multicast_list,
	.ndo_set_mac_address	= cvm_oct_common_set_mac_address,
	.ndo_do_ioctl		= cvm_oct_ioctl,
	.ndo_change_mtu		= cvm_oct_common_change_mtu,
	.ndo_get_stats		= cvm_oct_common_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= cvm_oct_poll_controller,
#endif
};
static const struct net_device_ops cvm_oct_sgmii_netdev_ops = {
	.ndo_init		= cvm_oct_sgmii_init,
	.ndo_uninit		= cvm_oct_sgmii_uninit,
	.ndo_open		= cvm_oct_sgmii_open,
	.ndo_stop		= cvm_oct_sgmii_stop,
	.ndo_start_xmit		= cvm_oct_xmit,
	.ndo_set_multicast_list	= cvm_oct_common_set_multicast_list,
	.ndo_set_mac_address	= cvm_oct_common_set_mac_address,
	.ndo_do_ioctl		= cvm_oct_ioctl,
	.ndo_change_mtu		= cvm_oct_common_change_mtu,
	.ndo_get_stats		= cvm_oct_common_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= cvm_oct_poll_controller,
#endif
};
static const struct net_device_ops cvm_oct_spi_netdev_ops = {
	.ndo_init		= cvm_oct_spi_init,
	.ndo_uninit		= cvm_oct_spi_uninit,
	.ndo_start_xmit		= cvm_oct_xmit,
	.ndo_set_multicast_list	= cvm_oct_common_set_multicast_list,
	.ndo_set_mac_address	= cvm_oct_common_set_mac_address,
	.ndo_do_ioctl		= cvm_oct_ioctl,
	.ndo_change_mtu		= cvm_oct_common_change_mtu,
	.ndo_get_stats		= cvm_oct_common_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= cvm_oct_poll_controller,
#endif
};
static const struct net_device_ops cvm_oct_rgmii_netdev_ops = {
	.ndo_init		= cvm_oct_rgmii_init,
	.ndo_uninit		= cvm_oct_rgmii_uninit,
	.ndo_open		= cvm_oct_rgmii_open,
	.ndo_stop		= cvm_oct_rgmii_stop,
	.ndo_start_xmit		= cvm_oct_xmit,
	.ndo_set_multicast_list	= cvm_oct_common_set_multicast_list,
	.ndo_set_mac_address	= cvm_oct_common_set_mac_address,
	.ndo_do_ioctl		= cvm_oct_ioctl,
	.ndo_change_mtu		= cvm_oct_common_change_mtu,
	.ndo_get_stats		= cvm_oct_common_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= cvm_oct_poll_controller,
#endif
};
static const struct net_device_ops cvm_oct_pow_netdev_ops = {
	.ndo_init		= cvm_oct_common_init,
	.ndo_start_xmit		= cvm_oct_xmit_pow,
	.ndo_set_multicast_list	= cvm_oct_common_set_multicast_list,
	.ndo_set_mac_address	= cvm_oct_common_set_mac_address,
	.ndo_do_ioctl		= cvm_oct_ioctl,
	.ndo_change_mtu		= cvm_oct_common_change_mtu,
	.ndo_get_stats		= cvm_oct_common_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= cvm_oct_poll_controller,
#endif
};

/**
 * Module/ driver initialization. Creates the linux network
 * devices.
 *
 * Returns Zero on success
 */
static int __init cvm_oct_init_module(void)
{
	int num_interfaces;
	int interface;
	int fau = FAU_NUM_PACKET_BUFFERS_TO_FREE;
	int qos;

	pr_notice("cavium-ethernet %s\n", OCTEON_ETHERNET_VERSION);

	cvm_oct_proc_initialize();
	cvm_oct_rx_initialize();
	cvm_oct_configure_common_hw();

	cvmx_helper_initialize_packet_io_global();

	/* Change the input group for all ports before input is enabled */
	num_interfaces = cvmx_helper_get_number_of_interfaces();
	for (interface = 0; interface < num_interfaces; interface++) {
		int num_ports = cvmx_helper_ports_on_interface(interface);
		int port;

		for (port = cvmx_helper_get_ipd_port(interface, 0);
		     port < cvmx_helper_get_ipd_port(interface, num_ports);
		     port++) {
			union cvmx_pip_prt_tagx pip_prt_tagx;
			pip_prt_tagx.u64 =
			    cvmx_read_csr(CVMX_PIP_PRT_TAGX(port));
			pip_prt_tagx.s.grp = pow_receive_group;
			cvmx_write_csr(CVMX_PIP_PRT_TAGX(port),
				       pip_prt_tagx.u64);
		}
	}

	cvmx_helper_ipd_and_packet_input_enable();

	memset(cvm_oct_device, 0, sizeof(cvm_oct_device));

	/*
	 * Initialize the FAU used for counting packet buffers that
	 * need to be freed.
	 */
	cvmx_fau_atomic_write32(FAU_NUM_PACKET_BUFFERS_TO_FREE, 0);

	if ((pow_send_group != -1)) {
		struct net_device *dev;
		pr_info("\tConfiguring device for POW only access\n");
		dev = alloc_etherdev(sizeof(struct octeon_ethernet));
		if (dev) {
			/* Initialize the device private structure. */
			struct octeon_ethernet *priv = netdev_priv(dev);
			memset(priv, 0, sizeof(struct octeon_ethernet));

			dev->netdev_ops = &cvm_oct_pow_netdev_ops;
			priv->imode = CVMX_HELPER_INTERFACE_MODE_DISABLED;
			priv->port = CVMX_PIP_NUM_INPUT_PORTS;
			priv->queue = -1;
			strcpy(dev->name, "pow%d");
			for (qos = 0; qos < 16; qos++)
				skb_queue_head_init(&priv->tx_free_list[qos]);

			if (register_netdev(dev) < 0) {
				pr_err("Failed to register ethernet "
					 "device for POW\n");
				kfree(dev);
			} else {
				cvm_oct_device[CVMX_PIP_NUM_INPUT_PORTS] = dev;
				pr_info("%s: POW send group %d, receive "
					"group %d\n",
				     dev->name, pow_send_group,
				     pow_receive_group);
			}
		} else {
			pr_err("Failed to allocate ethernet device "
				 "for POW\n");
		}
	}

	num_interfaces = cvmx_helper_get_number_of_interfaces();
	for (interface = 0; interface < num_interfaces; interface++) {
		cvmx_helper_interface_mode_t imode =
		    cvmx_helper_interface_get_mode(interface);
		int num_ports = cvmx_helper_ports_on_interface(interface);
		int port;

		for (port = cvmx_helper_get_ipd_port(interface, 0);
		     port < cvmx_helper_get_ipd_port(interface, num_ports);
		     port++) {
			struct octeon_ethernet *priv;
			struct net_device *dev =
			    alloc_etherdev(sizeof(struct octeon_ethernet));
			if (!dev) {
				pr_err("Failed to allocate ethernet device "
					 "for port %d\n", port);
				continue;
			}
			if (disable_core_queueing)
				dev->tx_queue_len = 0;

			/* Initialize the device private structure. */
			priv = netdev_priv(dev);
			memset(priv, 0, sizeof(struct octeon_ethernet));

			priv->imode = imode;
			priv->port = port;
			priv->queue = cvmx_pko_get_base_queue(priv->port);
			priv->fau = fau - cvmx_pko_get_num_queues(port) * 4;
			for (qos = 0; qos < 16; qos++)
				skb_queue_head_init(&priv->tx_free_list[qos]);
			for (qos = 0; qos < cvmx_pko_get_num_queues(port);
			     qos++)
				cvmx_fau_atomic_write32(priv->fau + qos * 4, 0);

			switch (priv->imode) {

			/* These types don't support ports to IPD/PKO */
			case CVMX_HELPER_INTERFACE_MODE_DISABLED:
			case CVMX_HELPER_INTERFACE_MODE_PCIE:
			case CVMX_HELPER_INTERFACE_MODE_PICMG:
				break;

			case CVMX_HELPER_INTERFACE_MODE_NPI:
				dev->netdev_ops = &cvm_oct_npi_netdev_ops;
				strcpy(dev->name, "npi%d");
				break;

			case CVMX_HELPER_INTERFACE_MODE_XAUI:
				dev->netdev_ops = &cvm_oct_xaui_netdev_ops;
				strcpy(dev->name, "xaui%d");
				break;

			case CVMX_HELPER_INTERFACE_MODE_LOOP:
				dev->netdev_ops = &cvm_oct_npi_netdev_ops;
				strcpy(dev->name, "loop%d");
				break;

			case CVMX_HELPER_INTERFACE_MODE_SGMII:
				dev->netdev_ops = &cvm_oct_sgmii_netdev_ops;
				strcpy(dev->name, "eth%d");
				break;

			case CVMX_HELPER_INTERFACE_MODE_SPI:
				dev->netdev_ops = &cvm_oct_spi_netdev_ops;
				strcpy(dev->name, "spi%d");
				break;

			case CVMX_HELPER_INTERFACE_MODE_RGMII:
			case CVMX_HELPER_INTERFACE_MODE_GMII:
				dev->netdev_ops = &cvm_oct_rgmii_netdev_ops;
				strcpy(dev->name, "eth%d");
				break;
			}

			if (!dev->netdev_ops) {
				kfree(dev);
			} else if (register_netdev(dev) < 0) {
				pr_err("Failed to register ethernet device "
					 "for interface %d, port %d\n",
					 interface, priv->port);
				kfree(dev);
			} else {
				cvm_oct_device[priv->port] = dev;
				fau -=
				    cvmx_pko_get_num_queues(priv->port) *
				    sizeof(uint32_t);
			}
		}
	}

	if (INTERRUPT_LIMIT) {
		/*
		 * Set the POW timer rate to give an interrupt at most
		 * INTERRUPT_LIMIT times per second.
		 */
		cvmx_write_csr(CVMX_POW_WQ_INT_PC,
			       octeon_bootinfo->eclock_hz / (INTERRUPT_LIMIT *
							     16 * 256) << 8);

		/*
		 * Enable POW timer interrupt. It will count when
		 * there are packets available.
		 */
		cvmx_write_csr(CVMX_POW_WQ_INT_THRX(pow_receive_group),
			       0x1ful << 24);
	} else {
		/* Enable POW interrupt when our port has at least one packet */
		cvmx_write_csr(CVMX_POW_WQ_INT_THRX(pow_receive_group), 0x1001);
	}

	/* Enable the poll timer for checking RGMII status */
	init_timer(&cvm_oct_poll_timer);
	cvm_oct_poll_timer.data = 0;
	cvm_oct_poll_timer.function = cvm_do_timer;
	mod_timer(&cvm_oct_poll_timer, jiffies + HZ);

	return 0;
}

/**
 * Module / driver shutdown
 *
 * Returns Zero on success
 */
static void __exit cvm_oct_cleanup_module(void)
{
	int port;

	/* Disable POW interrupt */
	cvmx_write_csr(CVMX_POW_WQ_INT_THRX(pow_receive_group), 0);

	cvmx_ipd_disable();

	/* Free the interrupt handler */
	free_irq(OCTEON_IRQ_WORKQ0 + pow_receive_group, cvm_oct_device);

	del_timer(&cvm_oct_poll_timer);
	cvm_oct_rx_shutdown();
	cvmx_pko_disable();

	/* Free the ethernet devices */
	for (port = 0; port < TOTAL_NUMBER_OF_PORTS; port++) {
		if (cvm_oct_device[port]) {
			cvm_oct_tx_shutdown(cvm_oct_device[port]);
			unregister_netdev(cvm_oct_device[port]);
			kfree(cvm_oct_device[port]);
			cvm_oct_device[port] = NULL;
		}
	}

	cvmx_pko_shutdown();
	cvm_oct_proc_shutdown();

	cvmx_ipd_free_ptr();

	/* Free the HW pools */
	cvm_oct_mem_empty_fpa(CVMX_FPA_PACKET_POOL, CVMX_FPA_PACKET_POOL_SIZE,
			      num_packet_buffers);
	cvm_oct_mem_empty_fpa(CVMX_FPA_WQE_POOL, CVMX_FPA_WQE_POOL_SIZE,
			      num_packet_buffers);
	if (CVMX_FPA_OUTPUT_BUFFER_POOL != CVMX_FPA_PACKET_POOL)
		cvm_oct_mem_empty_fpa(CVMX_FPA_OUTPUT_BUFFER_POOL,
				      CVMX_FPA_OUTPUT_BUFFER_POOL_SIZE, 128);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium Networks <support@caviumnetworks.com>");
MODULE_DESCRIPTION("Cavium Networks Octeon ethernet driver.");
module_init(cvm_oct_init_module);
module_exit(cvm_oct_cleanup_module);
