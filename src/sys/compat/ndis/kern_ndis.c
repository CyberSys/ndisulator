/*-
 * Copyright (c) 2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/errno.h>
#include <sys/callout.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/conf.h>

#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/kthread.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <compat/ndis/pe_var.h>
#include <compat/ndis/cfg_var.h>
#include <compat/ndis/resource_var.h>
#include <compat/ndis/ntoskrnl_var.h>
#include <compat/ndis/ndis_var.h>
#include <compat/ndis/hal_var.h>
#include <compat/ndis/usbd_var.h>
#include <dev/if_ndis/if_ndisvar.h>

#define	NDIS_DUMMY_PATH "\\\\some\\bogus\\path"

static void	ndis_status_func(ndis_handle, ndis_status, void *, uint32_t);
static void	ndis_status_done_func(ndis_handle);
static void	ndis_set_done_func(ndis_handle, ndis_status);
static void	ndis_get_done_func(ndis_handle, ndis_status);
static void	ndis_reset_done_func(ndis_handle, ndis_status, uint8_t);
static void	ndis_send_rsrcavail_func(ndis_handle);
static void	ndis_interrupt_setup(kdpc *, device_object *, irp *,
		    struct ndis_softc *);
static void	ndis_return_packet_nic(device_object *, void *);

static image_patch_table kernndis_functbl[] = {
	IMPORT_SFUNC(ndis_status_func, 4),
	IMPORT_SFUNC(ndis_status_done_func, 1),
	IMPORT_SFUNC(ndis_set_done_func, 2),
	IMPORT_SFUNC(ndis_get_done_func, 2),
	IMPORT_SFUNC(ndis_reset_done_func, 3),
	IMPORT_SFUNC(ndis_send_rsrcavail_func, 1),
	IMPORT_SFUNC(ndis_interrupt_setup, 4),
	IMPORT_SFUNC(ndis_return_packet_nic, 1),
	{ NULL, NULL, NULL }
};

static struct nd_head ndis_devhead;

MALLOC_DEFINE(M_NDIS_KERN, "ndis_kern", "ndis_kern buffers");

/*
 * This allows us to export our symbols to other modules.
 * Note that we call ourselves 'ndisapi' to avoid a namespace
 * collision with if_ndis.ko, which internally calls itself
 * 'ndis.'
 *
 * Note: some of the subsystems depend on each other, so the
 * order in which they're started is important. The order of
 * importance is:
 *
 * HAL - spinlocks and IRQL manipulation
 * ntoskrnl - DPC and workitem threads, object waiting
 * windrv - driver/device registration
 *
 * The HAL should also be the last thing shut down, since
 * the ntoskrnl subsystem will use spinlocks right up until
 * the DPC and workitem threads are terminated.
 */
static int
ndis_modevent(module_t mod, int cmd, void *arg)
{
	image_patch_table *patch;

	switch (cmd) {
	case MOD_LOAD:
		/* Initialize subsystems */
		hal_libinit();
		ntoskrnl_libinit();
		windrv_libinit();
		ndis_libinit();
		usbd_libinit();

		patch = kernndis_functbl;
		while (patch->ipt_func != NULL) {
			windrv_wrap((funcptr)patch->ipt_func,
			    (funcptr *)&patch->ipt_wrap,
			    patch->ipt_argcnt, patch->ipt_ftype);
			patch++;
		}

		TAILQ_INIT(&ndis_devhead);
		break;
	case MOD_SHUTDOWN:
		if (TAILQ_FIRST(&ndis_devhead) == NULL) {
			/* Shut down subsystems */
			usbd_libfini();
			ndis_libfini();
			windrv_libfini();
			ntoskrnl_libfini();
			hal_libfini();

			patch = kernndis_functbl;
			while (patch->ipt_func != NULL) {
				windrv_unwrap(patch->ipt_wrap);
				patch++;
			}
		}
		break;
	case MOD_UNLOAD:
		/* Shut down subsystems */
		usbd_libfini();
		ndis_libfini();
		windrv_libfini();
		ntoskrnl_libfini();
		hal_libfini();

		patch = kernndis_functbl;
		while (patch->ipt_func != NULL) {
			windrv_unwrap(patch->ipt_wrap);
			patch++;
		}
		break;
	default:
		return (EINVAL);
	}

	return (0);
}
DEV_MODULE(ndisapi, ndis_modevent, NULL);
MODULE_VERSION(ndisapi, 1);

static void
ndis_send_rsrcavail_func(ndis_handle adapter)
{
}

static void
ndis_status_func(ndis_handle adapter, ndis_status status, void *sbuf,
    uint32_t slen)
{
}

static void
ndis_status_done_func(ndis_handle adapter)
{
}

static void
ndis_set_done_func(ndis_handle adapter, ndis_status status)
{
	ndis_miniport_block *block;

	block = adapter;
	block->nmb_setstat = status;

	KeSetEvent(&block->nmb_setevent, IO_NO_INCREMENT, FALSE);
}

static void
ndis_get_done_func(ndis_handle adapter, ndis_status status)
{
	ndis_miniport_block *block;

	block = adapter;
	block->nmb_getstat = status;

	KeSetEvent(&block->nmb_getevent, IO_NO_INCREMENT, FALSE);
}

static void
ndis_reset_done_func(ndis_handle adapter, ndis_status status,
    uint8_t addressingreset)
{
	ndis_miniport_block *block;
	struct ndis_softc *sc;

	block = adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);

	KeSetEvent(&block->nmb_resetevent, IO_NO_INCREMENT, FALSE);
}

void
ndis_create_sysctls(void *arg)
{
	struct ndis_softc *sc = arg;
	ndis_cfg *vals;
	char buf[256];
	struct sysctl_oid *oidp;
	struct sysctl_ctx_entry *e;

	vals = sc->ndis_regvals;

	TAILQ_INIT(&sc->ndis_cfglist_head);

	/* Add the driver-specific registry keys. */
	for (;;) {
		if (vals->nc_cfgkey == NULL)
			break;
		if (vals->nc_idx != sc->ndis_devidx) {
			vals++;
			continue;
		}

		/* See if we already have a sysctl with this name */
		oidp = NULL;
		TAILQ_FOREACH(e, device_get_sysctl_ctx(sc->ndis_dev), link) {
			oidp = e->entry;
			if (strcasecmp(oidp->oid_name, vals->nc_cfgkey) == 0)
				break;
			oidp = NULL;
		}
		if (oidp != NULL) {
			vals++;
			continue;
		}

		ndis_add_sysctl(sc, vals->nc_cfgkey, vals->nc_cfgdesc,
		    vals->nc_val, CTLFLAG_RW);
		vals++;
	}

	/* Now add a couple of builtin keys. */
	/*
	 * Environment can be either Windows (0) or WindowsNT (1).
	 * We qualify as the latter.
	 */
	ndis_add_sysctl(sc, "Environment",
	    "Windows environment", "1", CTLFLAG_RD);
	/* NDIS version should be 5.1. */
	ndis_add_sysctl(sc, "NdisVersion",
	    "NDIS API Version", "0x00050001", CTLFLAG_RD);
	/* Bus type (PCI, PCMCIA, etc...) */
	sprintf(buf, "%d", (int)sc->ndis_iftype);
	ndis_add_sysctl(sc, "BusType", "Bus Type", buf, CTLFLAG_RD);
	if (sc->ndis_res_io != NULL) {
		sprintf(buf, "0x%lx", rman_get_start(sc->ndis_res_io));
		ndis_add_sysctl(sc, "IOBaseAddress",
		    "Base I/O Address", buf, CTLFLAG_RD);
	}
	if (sc->ndis_irq != NULL) {
		sprintf(buf, "%lu", rman_get_start(sc->ndis_irq));
		ndis_add_sysctl(sc, "InterruptNumber",
		    "Interrupt Number", buf, CTLFLAG_RD);
	}
}

int
ndis_add_sysctl(void *arg, char *key, char *desc, char *val, int flag)
{
	struct ndis_softc *sc = arg;
	struct ndis_cfglist *cfg;
	char descstr[256];

	cfg = malloc(sizeof(struct ndis_cfglist), M_NDIS_KERN, M_NOWAIT|M_ZERO);
	if (cfg == NULL)
		return (ENOMEM);
	cfg->ndis_cfg.nc_cfgkey = strdup(key, M_NDIS_KERN);
	if (desc == NULL) {
		snprintf(descstr, sizeof(descstr), "%s (dynamic)", key);
		cfg->ndis_cfg.nc_cfgdesc = strdup(descstr, M_NDIS_KERN);
	} else
		cfg->ndis_cfg.nc_cfgdesc = strdup(desc, M_NDIS_KERN);
	strcpy(cfg->ndis_cfg.nc_val, val);

	TAILQ_INSERT_TAIL(&sc->ndis_cfglist_head, cfg, link);

	cfg->ndis_oid = SYSCTL_ADD_STRING(device_get_sysctl_ctx(sc->ndis_dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->ndis_dev)),
	    OID_AUTO, cfg->ndis_cfg.nc_cfgkey, flag,
	    cfg->ndis_cfg.nc_val, sizeof(cfg->ndis_cfg.nc_val),
	    cfg->ndis_cfg.nc_cfgdesc);

	return (0);
}

void
ndis_flush_sysctls(void *arg)
{
	struct ndis_softc *sc = arg;
	struct ndis_cfglist *cfg;
	struct sysctl_ctx_list *clist;

	clist = device_get_sysctl_ctx(sc->ndis_dev);

	while (!TAILQ_EMPTY(&sc->ndis_cfglist_head)) {
		cfg = TAILQ_FIRST(&sc->ndis_cfglist_head);
		TAILQ_REMOVE(&sc->ndis_cfglist_head, cfg, link);
		sysctl_ctx_entry_del(clist, cfg->ndis_oid);
		sysctl_remove_oid(cfg->ndis_oid, 1, 0);
		free(cfg->ndis_cfg.nc_cfgkey, M_NDIS_KERN);
		free(cfg->ndis_cfg.nc_cfgdesc, M_NDIS_KERN);
		free(cfg, M_NDIS_KERN);
	}
}

static void
ndis_return_packet_nic(device_object *dobj, void *arg)
{
	ndis_miniport_block *block = arg;
	ndis_miniport_driver_characteristics *ch;
	ndis_packet *p;
	uint8_t irql;
	list_entry *l;

	KASSERT(block != NULL, ("no block"));
	KASSERT(block->nmb_miniport_adapter_ctx != NULL, ("no adapter"));
	ch = IoGetDriverObjectExtension(dobj->do_drvobj, (void *)1);
	KASSERT(ch->nmc_return_packet_func != NULL, ("no return_packet"));
	KeAcquireSpinLock(&block->nmb_returnlock, &irql);
	while (!IsListEmpty(&block->nmb_returnlist)) {
		l = RemoveHeadList((&block->nmb_returnlist));
		p = CONTAINING_RECORD(l, ndis_packet, np_list);
		InitializeListHead((&p->np_list));
		KeReleaseSpinLock(&block->nmb_returnlock, irql);
		MSCALL2(ch->nmc_return_packet_func,
		block->nmb_miniport_adapter_ctx, p);
		KeAcquireSpinLock(&block->nmb_returnlock, &irql);
	}
	KeReleaseSpinLock(&block->nmb_returnlock, irql);
}

void
ndis_return_packet(void *buf, void *arg)
{
	ndis_packet *p = arg;
	ndis_miniport_block *block;

	p->np_refcnt--;
	if (p->np_refcnt)
		return;

	block = ((struct ndis_softc *)p->np_softc)->ndis_block;
	KeAcquireSpinLockAtDpcLevel(&block->nmb_returnlock);
	InitializeListHead((&p->np_list));
	InsertHeadList((&block->nmb_returnlist), (&p->np_list));
	KeReleaseSpinLockFromDpcLevel(&block->nmb_returnlock);

	IoQueueWorkItem(block->nmb_returnitem,
	    (io_workitem_func)kernndis_functbl[7].ipt_wrap,
	    WORKQUEUE_CRITICAL, block);
}

void
ndis_free_bufs(ndis_buffer *b0)
{
	ndis_buffer *next;

	if (b0 == NULL)
		return;

	while (b0 != NULL) {
		next = b0->mdl_next;
		IoFreeMdl(b0);
		b0 = next;
	}
}

void
ndis_free_packet(ndis_packet *p)
{
	KASSERT(p != NULL, ("no packet"));
	ndis_free_bufs(p->np_private.npp_head);
	NdisFreePacket(p);
}

int
ndis_convert_res(void *arg)
{
	struct ndis_softc *sc = arg;
	ndis_resource_list *rl = NULL;
	cm_partial_resource_desc *prd = NULL;
	ndis_miniport_block *block;
	device_t dev;
	struct resource_list *brl;
	struct resource_list_entry *brle;

	block = sc->ndis_block;
	dev = sc->ndis_dev;

	rl = malloc(sizeof(ndis_resource_list) +
	    (sizeof(cm_partial_resource_desc) * (sc->ndis_rescnt - 1)),
	    M_NDIS_KERN, M_NOWAIT|M_ZERO);
	if (rl == NULL)
		return (ENOMEM);

	rl->cprl_version = 5;
	rl->cprl_version = 1;
	rl->cprl_count = sc->ndis_rescnt;
	prd = rl->cprl_partial_descs;

	brl = BUS_GET_RESOURCE_LIST(dev, dev);
	if (brl != NULL) {
		STAILQ_FOREACH(brle, brl, link) {
			switch (brle->type) {
			case SYS_RES_IOPORT:
				prd->cprd_type = CmResourceTypePort;
				prd->cprd_flags = CM_RESOURCE_PORT_IO;
				prd->cprd_sharedisp =
				    CM_RESOURCE_SHARE_DEVICE_EXCLUSIVE;
				prd->u.cprd_port.cprd_start.np_quad =
				    brle->start;
				prd->u.cprd_port.cprd_len = brle->count;
				break;
			case SYS_RES_MEMORY:
				prd->cprd_type = CmResourceTypeMemory;
				prd->cprd_flags =
				    CM_RESOURCE_MEMORY_READ_WRITE;
				prd->cprd_sharedisp =
				    CM_RESOURCE_SHARE_DEVICE_EXCLUSIVE;
				prd->u.cprd_mem.cprd_start.np_quad =
				    brle->start;
				prd->u.cprd_mem.cprd_len = brle->count;
				break;
			case SYS_RES_IRQ:
				prd->cprd_type = CmResourceTypeInterrupt;
				prd->cprd_flags = 0;
				/*
				 * Always mark interrupt resources as
				 * shared, since in our implementation,
				 * they will be.
				 */
				prd->cprd_sharedisp = CM_RESOURCE_SHARE_SHARED;
				prd->u.cprd_intr.cprd_level = brle->start;
				prd->u.cprd_intr.cprd_vector = brle->start;
				prd->u.cprd_intr.cprd_affinity = 0;
				break;
			default:
				break;
			}
			prd++;
		}
	}

	block->nmb_rlist = rl;

	return (0);
}

/*
 * Map an NDIS packet to an mbuf list. When an NDIS driver receives a
 * packet, it will hand it to us in the form of an ndis_packet,
 * which we need to convert to an mbuf that is then handed off
 * to the stack. Note: we configure the mbuf list so that it uses
 * the memory regions specified by the ndis_buffer structures in
 * the ndis_packet as external storage. In most cases, this will
 * point to a memory region allocated by the driver (either by
 * ndis_malloc_withtag() or ndis_alloc_sharedmem()). We expect
 * the driver to handle free()ing this region for is, so we set up
 * a dummy no-op free handler for it.
 */
int
ndis_ptom(struct mbuf **m0, ndis_packet *p)
{
	struct mbuf *m = NULL, *prev = NULL;
	ndis_buffer *buf;
	ndis_packet_private *priv;
	uint32_t totlen = 0;
	struct ifnet *ifp;
	struct ether_header *eh;
	int diff;

	if (p == NULL || m0 == NULL)
		return (EINVAL);

	priv = &p->np_private;
	buf = priv->npp_head;
	p->np_refcnt = 0;

	for (buf = priv->npp_head; buf != NULL; buf = buf->mdl_next) {
		if (buf == priv->npp_head)
#ifdef MT_HEADER
			MGETHDR(m, M_DONTWAIT, MT_HEADER);
#else
			MGETHDR(m, M_DONTWAIT, MT_DATA);
#endif
		else
			MGET(m, M_DONTWAIT, MT_DATA);
		if (m == NULL) {
			m_freem(*m0);
			*m0 = NULL;
			return (ENOBUFS);
		}
		m->m_len = MmGetMdlByteCount(buf);
		m->m_data = MmGetMdlVirtualAddress(buf);
		MEXTADD(m, m->m_data, m->m_len, ndis_return_packet,
		    m->m_data, p, 0, EXT_NDIS);
		p->np_refcnt++;

		totlen += m->m_len;
		if (m->m_flags & M_PKTHDR)
			*m0 = m;
		else
			prev->m_next = m;
		prev = m;
	}

	/*
	 * This is a hack to deal with the Marvell 8335 driver
	 * which, when associated with an AP in WPA-PSK mode,
	 * seems to overpad its frames by 8 bytes. I don't know
	 * that the extra 8 bytes are for, and they're not there
	 * in open mode, so for now clamp the frame size at 1514
	 * until I can figure out how to deal with this properly,
	 * otherwise if_ethersubr() will spank us by discarding
	 * the 'oversize' frames.
	 */
	eh = mtod((*m0), struct ether_header *);
	ifp = ((struct ndis_softc *)p->np_softc)->ndis_ifp;
	if (totlen > ETHER_MAX_FRAME(ifp, eh->ether_type, FALSE)) {
		diff = totlen - ETHER_MAX_FRAME(ifp, eh->ether_type, FALSE);
		totlen -= diff;
		m->m_len -= diff;
	}
	(*m0)->m_pkthdr.len = totlen;

	return (0);
}

/*
 * Create an NDIS packet from an mbuf chain.
 * This is used mainly when transmitting packets, where we need
 * to turn an mbuf off an interface's send queue and transform it
 * into an NDIS packet which will be fed into the NDIS driver's
 * send routine.
 *
 * NDIS packets consist of two parts: an ndis_packet structure,
 * which is vaguely analagous to the pkthdr portion of an mbuf,
 * and one or more ndis_buffer structures, which define the
 * actual memory segments in which the packet data resides.
 * We need to allocate one ndis_buffer for each mbuf in a chain,
 * plus one ndis_packet as the header.
 */
int
ndis_mtop(struct mbuf *m0, ndis_packet **p)
{
	struct mbuf *m;
	ndis_buffer *buf = NULL, *prev = NULL;
	ndis_packet_private *priv;

	if (p == NULL || *p == NULL || m0 == NULL)
		return (EINVAL);

	priv = &(*p)->np_private;
	priv->npp_totlen = m0->m_pkthdr.len;

	for (m = m0; m != NULL; m = m->m_next) {
		if (m->m_len == 0)
			continue;
		buf = IoAllocateMdl(m->m_data, m->m_len, FALSE, FALSE, NULL);
		if (buf == NULL) {
			ndis_free_packet(*p);
			*p = NULL;
			return (ENOMEM);
		}
		MmBuildMdlForNonPagedPool(buf);

		if (priv->npp_head == NULL)
			priv->npp_head = buf;
		else
			prev->mdl_next = buf;
		prev = buf;
	}

	priv->npp_tail = buf;

	return (0);
}

static int
ndis_request_info(uint32_t request, void *arg, ndis_oid oid, void *buf,
    uint32_t buflen, uint32_t *written, uint32_t *needed)
{
	struct ndis_softc *sc = arg;
	uint64_t duetime;
	ndis_status rval;
	uint32_t w = 0, n = 0;
	uint8_t irql;

	if (!written)
		written = &w;
	if (!needed)
		needed = &n;
	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_chars != NULL, ("no ndis_chars"));
	KASSERT(sc->ndis_chars->nmc_query_info_func != NULL, ("no query_info"));
	KASSERT(sc->ndis_chars->nmc_set_info_func != NULL, ("no set_info"));
	/*
	 * According to the NDIS spec, MiniportQueryInformation()
	 * and MiniportSetInformation() requests are handled serially:
	 * once one request has been issued, we must wait for it to
	 * finish before allowing another request to proceed.
	 */
	if (request == NDIS_REQUEST_QUERY_INFORMATION) {
		KeResetEvent(&sc->ndis_block->nmb_getevent);
		KeAcquireSpinLock(&sc->ndis_block->nmb_lock, &irql);
		rval = MSCALL6(sc->ndis_chars->nmc_query_info_func,
		    sc->ndis_block->nmb_miniport_adapter_ctx,
		    oid, buf, buflen, written, needed);
		KeReleaseSpinLock(&sc->ndis_block->nmb_lock, irql);
		if (rval == NDIS_STATUS_PENDING) {
			duetime = (5 * 1000000) * -10;
			KeWaitForSingleObject(&sc->ndis_block->nmb_getevent,
			    0, 0, FALSE, &duetime);
			rval = sc->ndis_block->nmb_getstat;
		}
	} else if (request == NDIS_REQUEST_SET_INFORMATION) {
		KeResetEvent(&sc->ndis_block->nmb_setevent);
		KeAcquireSpinLock(&sc->ndis_block->nmb_lock, &irql);
		rval = MSCALL6(sc->ndis_chars->nmc_set_info_func,
		    sc->ndis_block->nmb_miniport_adapter_ctx,
		    oid, buf, buflen, written, needed);
		KeReleaseSpinLock(&sc->ndis_block->nmb_lock, irql);
		if (rval == NDIS_STATUS_PENDING) {
			duetime = (5 * 1000000) * -10;
			KeWaitForSingleObject(&sc->ndis_block->nmb_setevent,
			    0, 0, FALSE, &duetime);
			rval = sc->ndis_block->nmb_setstat;
		}
	} else
		return (NDIS_STATUS_NOT_SUPPORTED);

	return (rval);
}

inline int
ndis_get(void *arg, ndis_oid oid, void *val, uint32_t len)
{
	return (ndis_request_info(NDIS_REQUEST_QUERY_INFORMATION,
	    arg, oid, val, len, NULL, NULL));
}

inline int
ndis_get_int(void *arg, ndis_oid oid, uint32_t *val)
{
	return (ndis_request_info(NDIS_REQUEST_QUERY_INFORMATION,
	    arg, oid, val, sizeof(uint32_t), NULL, NULL));
}

inline int
ndis_get_info(void *arg, ndis_oid oid, void *buf, uint32_t buflen,
    uint32_t *written, uint32_t *needed)
{
	return (ndis_request_info(NDIS_REQUEST_QUERY_INFORMATION,
	    arg, oid, buf, buflen, written, needed));
}

inline int
ndis_set(void *arg, ndis_oid oid, void *val, uint32_t len)
{
	return (ndis_request_info(NDIS_REQUEST_SET_INFORMATION,
	    arg, oid, val, len, NULL, NULL));
}

inline int
ndis_set_int(void *arg, ndis_oid oid, uint32_t val)
{
	return (ndis_request_info(NDIS_REQUEST_SET_INFORMATION,
	    arg, oid, &val, sizeof(uint32_t), NULL, NULL));
}

inline int
ndis_set_info(void *arg, ndis_oid oid, void *buf, uint32_t buflen,
    uint32_t *written, uint32_t *needed)
{
	return (ndis_request_info(NDIS_REQUEST_SET_INFORMATION,
	    arg, oid, buf, buflen, written, needed));
}

typedef void (*ndis_send_done_func) (ndis_handle, ndis_packet *, ndis_status);

int
ndis_send_packets(void *arg, ndis_packet **packets, int cnt)
{
	struct ndis_softc *sc = arg;
	int i;
	ndis_packet *p;
	uint8_t irql = 0;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_block->nmb_send_done_func != NULL, ("no send_done"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_chars->nmc_send_multi_func != NULL, ("no send_multi"));
	if (NDIS_SERIALIZED(sc->ndis_block))
		KeAcquireSpinLock(&sc->ndis_block->nmb_lock, &irql);
	MSCALL3(sc->ndis_chars->nmc_send_multi_func,
	    sc->ndis_block->nmb_miniport_adapter_ctx, packets, cnt);
	for (i = 0; i < cnt; i++) {
		p = packets[i];
		/*
		 * Either the driver already handed the packet to
		 * ndis_txeof() due to a failure, or it wants to keep
		 * it and release it asynchronously later. Skip to the
		 * next one.
		 */
		if (p == NULL || p->np_oob.npo_status == NDIS_STATUS_PENDING)
			continue;
		MSCALL3(sc->ndis_block->nmb_send_done_func,
		    sc->ndis_block, p, p->np_oob.npo_status);
	}
	if (NDIS_SERIALIZED(sc->ndis_block))
		KeReleaseSpinLock(&sc->ndis_block->nmb_lock, irql);
	return (0);
}

int
ndis_send_packet(void *arg, ndis_packet *packet)
{
	struct ndis_softc *sc = arg;
	ndis_status status;
	uint8_t irql = 0;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_block->nmb_send_done_func != NULL, ("no send_done"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_chars->nmc_send_single_func != NULL,
	    ("no send_single"));
	if (NDIS_SERIALIZED(sc->ndis_block))
		KeAcquireSpinLock(&sc->ndis_block->nmb_lock, &irql);
	status = MSCALL3(sc->ndis_chars->nmc_send_single_func,
	    sc->ndis_block->nmb_miniport_adapter_ctx, packet,
	    packet->np_private.npp_flags);
	if (status == NDIS_STATUS_PENDING) {
		if (NDIS_SERIALIZED(sc->ndis_block))
			KeReleaseSpinLock(&sc->ndis_block->nmb_lock, irql);
		return (0);
	}
	MSCALL3(sc->ndis_block->nmb_send_done_func,
	    sc->ndis_block, packet, status);
	if (NDIS_SERIALIZED(sc->ndis_block))
		KeReleaseSpinLock(&sc->ndis_block->nmb_lock, irql);
	return (status);
}

int
ndis_init_dma(void *arg)
{
	struct ndis_softc *sc = arg;
	int i;

	sc->ndis_tmaps = malloc(sizeof(bus_dmamap_t) * sc->ndis_maxpkts,
	    M_NDIS_KERN, M_NOWAIT|M_ZERO);
	if (sc->ndis_tmaps == NULL)
		return (ENOMEM);
	for (i = 0; i < sc->ndis_maxpkts; i++) {
		if (bus_dmamap_create(sc->ndis_ttag, 0,
		    &sc->ndis_tmaps[i]) != 0) {
			free(sc->ndis_tmaps, M_NDIS_KERN);
			return (ENODEV);
		}
	}
	return (0);
}

void
ndis_destroy_dma(void *arg)
{
	struct ndis_softc *sc = arg;
	struct mbuf *m;
	ndis_packet *p = NULL;
	int i;

	for (i = 0; i < sc->ndis_maxpkts; i++) {
		if (sc->ndis_txarray[i] != NULL) {
			p = sc->ndis_txarray[i];
			m = (struct mbuf *)p->np_rsvd[1];
			if (m != NULL)
				m_freem(m);
			ndis_free_packet(sc->ndis_txarray[i]);
		}
		bus_dmamap_destroy(sc->ndis_ttag, sc->ndis_tmaps[i]);
	}
	free(sc->ndis_tmaps, M_NDIS_KERN);
	bus_dma_tag_destroy(sc->ndis_ttag);
}

int
ndis_reset_nic(void *arg)
{
	struct ndis_softc *sc = arg;
	uint8_t addressing_reset;
	int rval;
	uint8_t irql = 0;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_chars->nmc_reset_func != NULL, ("no reset"));
	KeResetEvent(&sc->ndis_block->nmb_resetevent);
	if (NDIS_SERIALIZED(sc->ndis_block))
		KeAcquireSpinLock(&sc->ndis_block->nmb_lock, &irql);
	rval = MSCALL2(sc->ndis_chars->nmc_reset_func,
	    &addressing_reset, sc->ndis_block->nmb_miniport_adapter_ctx);
	if (NDIS_SERIALIZED(sc->ndis_block))
		KeReleaseSpinLock(&sc->ndis_block->nmb_lock, irql);
	if (rval == NDIS_STATUS_PENDING) {
		KeWaitForSingleObject(&sc->ndis_block->nmb_resetevent,
		    0, 0, FALSE, NULL);
		rval = sc->ndis_block->nmb_reset_status;
	}
	return (rval);
}

uint8_t
ndis_check_for_hang_nic(void *arg)
{
	struct ndis_softc *sc = arg;

	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	if (sc->ndis_chars->nmc_check_hang_func == NULL)
		return (FALSE);
	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	return (MSCALL1(sc->ndis_chars->nmc_check_hang_func,
	    sc->ndis_block->nmb_miniport_adapter_ctx));
}

void
ndis_disable_interrupts_nic(void *arg)
{
	struct ndis_softc *sc = arg;

	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	if (sc->ndis_chars->nmc_disable_interrupts_func != NULL)
		MSCALL1(sc->ndis_chars->nmc_disable_interrupts_func,
		    sc->ndis_block->nmb_miniport_adapter_ctx);
}

void
ndis_enable_interrupts_nic(void *arg)
{
	struct ndis_softc *sc = arg;

	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	if (sc->ndis_chars->nmc_enable_interrupts_func != NULL)
		MSCALL1(sc->ndis_chars->nmc_enable_interrupts_func,
		    sc->ndis_block->nmb_miniport_adapter_ctx);
}

void
ndis_halt_nic(void *arg)
{
	struct ndis_softc *sc = arg;

	if (!cold)
		KeFlushQueuedDpcs();
	NDIS_LOCK(sc);
	KASSERT(sc->ndis_block != NULL, ("no block"));
	sc->ndis_block->nmb_device_ctx = NULL;
	NDIS_UNLOCK(sc);
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_chars->nmc_halt_func != NULL, ("no halt"));
	MSCALL1(sc->ndis_chars->nmc_halt_func,
	    sc->ndis_block->nmb_miniport_adapter_ctx);
	NDIS_LOCK(sc);
	sc->ndis_block->nmb_miniport_adapter_ctx = NULL;
	NDIS_UNLOCK(sc);
}

void
ndis_shutdown_nic(void *arg)
{
	struct ndis_softc *sc = arg;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_chars->nmc_shutdown_func != NULL, ("no shutdown"));
	if (sc->ndis_chars->nmc_reserved0 == NULL)
		MSCALL1(sc->ndis_chars->nmc_shutdown_func,
		    sc->ndis_block->nmb_miniport_adapter_ctx);
	else
		MSCALL1(sc->ndis_chars->nmc_shutdown_func,
		    sc->ndis_chars->nmc_reserved0);
}

void
ndis_pnp_event_nic(void *arg, uint32_t event, uint32_t profile)
{
	struct ndis_softc *sc =  arg;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	if (sc->ndis_chars->nmc_pnp_event_notify_func == NULL)
		return;
	switch (event) {
	case NDIS_DEVICE_PNP_EVENT_SURPRISE_REMOVED:
		if (sc->ndis_block->nmb_flags &
		   NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK)
			MSCALL4(sc->ndis_chars->nmc_pnp_event_notify_func,
			sc->ndis_block->nmb_miniport_adapter_ctx,
			event, NULL, 0);
		break;
	case NDIS_DEVICE_PNP_EVENT_POWER_PROFILE_CHANGED:
		MSCALL4(sc->ndis_chars->nmc_pnp_event_notify_func,
		    sc->ndis_block->nmb_miniport_adapter_ctx,
		    event, &profile, sizeof(profile));
		break;
	default:
		break;
	}
}

int32_t
ndis_init_nic(void *arg)
{
	struct ndis_softc *sc = arg;
	ndis_status rval, status = 0;
	ndis_medium medium_array[] = { NDIS_MEDIUM_802_3 };
	uint32_t chosen_medium = 0;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_miniport_adapter_ctx != NULL,
	    ("no adapter"));
	KASSERT(sc->ndis_chars != NULL, ("no chars"));
	KASSERT(sc->ndis_chars->nmc_init_func != NULL, ("no init"));
	rval = MSCALL6(sc->ndis_chars->nmc_init_func, &status, &chosen_medium,
	    medium_array, sizeof(medium_array) / sizeof(medium_array[0]),
	    sc->ndis_block, sc->ndis_block);
	NDIS_LOCK(sc);
	if (rval)
		/*
		 * If the init fails, blow away the other exported routines
		 * we obtained from the driver so we can't call them later.
		 * If the init failed, none of these will work.
		 */
		sc->ndis_block->nmb_miniport_adapter_ctx = NULL;
	else
		sc->ndis_block->nmb_device_ctx = sc;
	NDIS_UNLOCK(sc);
	return (rval);
}

static void
ndis_interrupt_setup(kdpc *dpc, device_object *dobj, irp *ip,
    struct ndis_softc *sc)
{
	ndis_miniport_interrupt *intr;

	KASSERT(sc->ndis_block != NULL, ("no block"));
	KASSERT(sc->ndis_block->nmb_interrupt != NULL, ("no interrupt"));
	intr = sc->ndis_block->nmb_interrupt;
	KeAcquireSpinLockAtDpcLevel(&intr->ni_dpc_count_lock);
	KeResetEvent(&intr->ni_dpcs_completed_event);
	if (KeInsertQueueDpc(&intr->ni_interrupt_dpc, NULL, NULL) == TRUE)
		intr->ni_dpc_count++;
	KeReleaseSpinLockFromDpcLevel(&intr->ni_dpc_count_lock);
}

int32_t
NdisAddDevice(driver_object *drv, device_object *pdo)
{
	device_object *fdo;
	ndis_miniport_block *block;
	struct ndis_softc *sc;
	int32_t status;

	sc = device_get_softc(pdo->do_devext);
	if (sc->ndis_iftype == PCMCIABus || sc->ndis_iftype == PCIBus) {
		status = bus_setup_intr(sc->ndis_dev, sc->ndis_irq,
		    INTR_TYPE_NET|INTR_MPSAFE, NULL, ntoskrnl_intr, NULL,
		    &sc->ndis_intrhand);
		if (status) {
			device_printf(sc->ndis_dev, "couldn't setup"
			    "interrupt; (%d)\n", status);
			return (NDIS_STATUS_FAILURE);
		}
	}

	status = IoCreateDevice(drv, sizeof(ndis_miniport_block), NULL,
	    FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
	if (status != NDIS_STATUS_SUCCESS)
		return (status);

	block = fdo->do_devext;
	block->nmb_filter_dbs.nf_ethdb = block;
	block->nmb_deviceobj = fdo;
	block->nmb_physdeviceobj = pdo;
	block->nmb_nextdeviceobj = IoAttachDeviceToDeviceStack(fdo, pdo);
	KeInitializeSpinLock(&block->nmb_lock);
	KeInitializeSpinLock(&block->nmb_returnlock);
	KeInitializeEvent(&block->nmb_getevent, EVENT_TYPE_NOTIFY, TRUE);
	KeInitializeEvent(&block->nmb_setevent, EVENT_TYPE_NOTIFY, TRUE);
	KeInitializeEvent(&block->nmb_resetevent, EVENT_TYPE_NOTIFY, TRUE);
	InitializeListHead(&block->nmb_parmlist);
	InitializeListHead(&block->nmb_returnlist);
	block->nmb_returnitem = IoAllocateWorkItem(fdo);

	/*
	 * Stash pointers to the miniport block and miniport
	 * characteristics info in the if_ndis softc so the
	 * UNIX wrapper driver can get to them later.
	 */
	sc->ndis_block = block;
	sc->ndis_chars = IoGetDriverObjectExtension(drv, (void *)1);

	/*
	 * If the driver has a MiniportTransferData() function,
	 * we should allocate a private RX packet pool.
	 */
	if (sc->ndis_chars->nmc_transfer_data_func != NULL) {
		NdisAllocatePacketPool(&status, &block->nmb_rxpool,
		    32, PROTOCOL_RESERVED_SIZE_IN_PACKET);
		if (status != NDIS_STATUS_SUCCESS) {
			IoDetachDevice(block->nmb_nextdeviceobj);
			IoDeleteDevice(fdo);
			return (status);
		}
		InitializeListHead((&block->nmb_packet_list));
	}

	/* Give interrupt handling priority over timers. */
	IoInitializeDpcRequest(fdo, kernndis_functbl[6].ipt_wrap);
	KeSetImportanceDpc(&fdo->do_dpc, KDPC_IMPORTANCE_HIGH);

	/* Finish up BSD-specific setup. */
	block->nmb_signature = (void *)0xcafebabe;
	block->nmb_status_func = kernndis_functbl[0].ipt_wrap;
	block->nmb_status_done_func = kernndis_functbl[1].ipt_wrap;
	block->nmb_set_done_func = kernndis_functbl[2].ipt_wrap;
	block->nmb_query_done_func = kernndis_functbl[3].ipt_wrap;
	block->nmb_reset_done_func = kernndis_functbl[4].ipt_wrap;
	block->nmb_send_rsrc_func = kernndis_functbl[5].ipt_wrap;

	TAILQ_INSERT_TAIL(&ndis_devhead, block, link);

	return (NDIS_STATUS_SUCCESS);
}

void
ndis_unload_driver(void *arg)
{
	struct ndis_softc *sc = arg;
	device_object *fdo;

	if (sc->ndis_intrhand) /* FIXME: doesn't belong here */
		bus_teardown_intr(sc->ndis_dev,
		    sc->ndis_irq, sc->ndis_intrhand);

	if (sc->ndis_block->nmb_rlist != NULL)
		free(sc->ndis_block->nmb_rlist, M_NDIS_KERN);

	TAILQ_REMOVE(&ndis_devhead, sc->ndis_block, link);
	if (sc->ndis_chars->nmc_transfer_data_func != NULL)
		NdisFreePacketPool(sc->ndis_block->nmb_rxpool);
	fdo = sc->ndis_block->nmb_deviceobj;
	IoFreeWorkItem(sc->ndis_block->nmb_returnitem);
	IoDetachDevice(sc->ndis_block->nmb_nextdeviceobj);
	IoDeleteDevice(fdo);
}
