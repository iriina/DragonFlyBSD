/*
 * Copyright (c) 2006 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
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
 * 
 * $DragonFly: src/sys/platform/vkernel/platform/checkpoint.c, 2011/06/28 11:22:00 $
 */

#include <sys/types.h>
#include <sys/checkpoint.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/cons.h>
#include <sys/random.h>
#include <sys/vkernel.h>
#include <sys/tls.h>
#include <sys/reboot.h>
#include <sys/proc.h>
#include <sys/msgbuf.h>
#include <sys/vmspace.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <vm/vm_page.h>
#include <sys/mplock2.h>
#include <sys/sysref2.h>
#include <sys/kthread.h>

#include <vm/vm_extern.h>
#include <vm/pmap.h>

#include <machine/smp.h>
#include <machine/cpu.h>
#include <machine/globaldata.h>
#include <machine/tls.h>
#include <machine/cothread.h>
#include <machine/md_var.h>
#include <machine/vmparam.h>
#include <cpu/specialreg.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/tap/if_tap.h>
#include <net/bridge/if_bridgevar.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <assert.h>
#include <sysexits.h>

/**
 * Tests the network interfaces states save and restore
 * Doesn't do a real checkpoint (for the moment), only simulates it through 
 * SIGCKPT and SIGTHAW signals, closes and restores vke and tap interfaces
 * !! If the signals are caught by the idle thread, it creates and schedules another
 * kernel thread in order to deal with the checkpointing work
 */
#define IFNET_TEST 1

/**
 * Tries to remap vkm and the vprocs vmspaces
 */
//#define VMSPACE_TEST 1

static void ckptsig(int signo);
static void do_restore(void *arg);
static void do_checkpoint(void *arg);

#ifdef IFNET_TEST
static void thawsig(int signo);
static void ckpt_netif(void);
static void restore_netif(void);
static void netif_down(struct ifnet *ifp);
static struct ifnet *lookup_ifnet(int unit);
static int netif_del_tapbrg(int unit, char *bridge, int s);
#endif

#ifdef VMSPACE_TEST
static void ckpt_vmspaces(void);
static void restore_vmspaces(void);
static void recreate_vmspace(struct proc *);
static void recreate_kva(void);
#endif

struct if_task {
	void *arg;
	int id;
};

/*
 * Installs handlers for checkpoint signals
 */
void
init_checkpointing(void)
{ 
	struct sigaction sa;
	
	bzero(&sa, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = ckptsig;
	sigaction(SIGCKPT, &sa, NULL);

#ifdef IFNET_TEST
	struct sigaction ss;
		
	bzero(&ss, sizeof(ss));
	sigemptyset(&ss.sa_mask);
	ss.sa_handler = thawsig;
	sigaction(35, &ss, NULL);
#endif
}

#ifdef IFNET_TEST
/*
 * SIGTHAW signal handlers
 * XXX: Currently used only for testing network
 * interfaces checkpoint, not sure if it shall be kept in the code
 */
static void
thawsig(int signo)
{
	struct thread *td = curthread;

	if (td == &td->td_gd->gd_idlethread) {
		printf("I'm the idle thead!\n");
		struct if_task task;
		kthread_create(do_restore, &task,
			NULL, "thaw");
		lwkt_switch();
	} else {
		printf("Normal thread\n");
		do_restore(NULL);
	}
}
#endif

/*
 * SIGCKPT signal handler
 */
static void
ckptsig(int signo)
{
	printf("I was checkpointed :D\n");
	struct thread *td = curthread;

	if (td == &td->td_gd->gd_idlethread) {
		printf("I'm the idle thead!\n");
		struct if_task task;
		kthread_create(do_checkpoint, &task,
			NULL, "ckpt");
		lwkt_switch();
	} else {
		printf("Normal thread\n");
		do_checkpoint(NULL);
	}
}

/*
 * Does all the ckeckpoint work
 */
static void
do_checkpoint(void *arg)
{

#ifdef VMSPACE_TEST
	ckpt_vmspaces();
#endif
#ifdef IFNET_TEST
	ckpt_netif();
#endif 


#ifdef VMSPACE_TEST
	/* Sync memory image file */
	int r;
	msync((void*)KvaStart, KERNEL_KVA_SIZE, MS_SYNC);
	r = sys_checkpoint(CKPT_FREEZE, -1, -1, -1);
	if (r == 1) {
		do_restore();
	}
#endif 
}

/*
 * Does all the restore work
 */
static void
do_restore(void *arg)
{
#ifdef VMSPACE_TEST
	restore_vmspaces();
#endif
#ifdef IFNET_TEST
	restore_netif();
#endif 
}

#ifdef VMSPACE_TEST 
static void
ckpt_vmspaces(void)
{
	struct proc *p;
	
	p = LIST_FIRST(&allproc);
	PHOLD(p);
	printf("pdir %lu \n",vmspace_pmap(p->p_vmspace)->pm_pdirpte);
	PRELE(p);
}

static void
restore_vmspaces(void)
{
	struct proc *p;

	recreate_kva();
	printf("Success remap vmspace0\n");
	p = LIST_FIRST(&allproc);
	for (; p != NULL; p = LIST_NEXT(p, p_list)) {	
		PHOLD(p);
		printf("pt %i %p\n", p->p_pid, p->p_vmspace);
		recreate_vmspace(p);	
		PRELE(p);	
	}
}

/*
 * Recreates a virtual process's vmspace
 */
static void 
recreate_vmspace(struct proc *p)
{
	printf("Restoring vmspace for pid %i\n", p->p_pid);
	
	//vm_map_t map = &p->p_vmspace->vm_map;
		
	//	lwkt_gettoken(&vm_token);
	//	lwkt_gettoken(&vmspace_token);
	//	lwkt_gettoken(&vmobj_token);
	if (p->p_pid != 0) {
		printf("xxx pdir %lu \n",vmspace_pmap(p->p_vmspace)->pm_pdirpte);
		cpu_vmspace_alloc(p->p_vmspace);
	}

	//	lwkt_reltoken(&vm_token);
	//	lwkt_reltoken(&vmspace_token);
	//	lwkt_reltoken(&vmobj_token);
}

/*
 * Recreates vkernel's virtual memory (vmspace0)
 */
static void 
recreate_kva(void)
{
	void *base;
	void *try;

	try = (void *)KvaStart;
	base = NULL;
	base = mmap(try, KERNEL_KVA_SIZE, PROT_READ|PROT_WRITE,
			MAP_FILE|MAP_SHARED|MAP_VPAGETABLE,
			MemImageFd, 0);
	if (base != try) {
		err(1, "Unable to mmap() kernel virtual memory!");
		/* NOT REACHED */
	}
	madvise(base, KERNEL_KVA_SIZE, MADV_NOSYNC);

	printf("XXX KVM mapped at %p-%p\n", (void *)KvaStart, (void *)KvaEnd);
	mcontrol(base, KERNEL_KVA_SIZE, MADV_SETMAP,
			0 | VPTE_R | VPTE_W | VPTE_V);
}

#endif

#ifdef IFNET_TEST 

/*
 * Saves states for network interfaces
 */
static void
ckpt_netif()
{
	struct vknetif_info *info;
	struct ifnet	*ifp; 
	int s, i;

	s = socket(AF_INET, SOCK_DGRAM, 0);	/* for ioctl(SIOC) */
	if (s < 0)
		return;

	for (i = 0; i < NetifNum; ++i) {
		info = &NetifInfo[i];

		/* Find the associated pseudo interface
		 * if ifp == NULL no interface was attached to the tap 
	 	 * XXX Find a better solution for lookup vkes from tap */ 
		ifp = lookup_ifnet(info->netif_unit); 
		if (ifp == NULL) {	
			printf("[CKPT] Unable to find ifnet interface for vke%d\n",
					info->netif_unit);
		} else {
			netif_down(ifp);
		}

		/* Remove tap interface from the bridge if it is tied to one */
		if (info->is_bridged) {
			printf("[CKPT] Removing interface tap%d from %s\n",
				info->tap_unit, info->tap_bridge);
			if (netif_del_tapbrg(info->tap_unit, info->tap_bridge, s) != 0) {
				printf("[CKPT] Unable to remove interface tap%d from %s\n",
					info->tap_unit, info->tap_bridge);
				continue;
			}
		}

		/* Set tap interface down */
		printf("[CKPT] Setting tap%d interface down\n", info->tap_unit);
		if (netif_set_tapflags(info->tap_unit, -IFF_UP, s) != 0) {
			printf("[CKPT] Failed to bring down tap%d interface\n", info->tap_unit);
			continue;
		}

		/* Close tap file descriptor */
		close(info->tap_fd);
		printf("[CKPT] Interface tap%d was checkpointed\n\n", info->tap_unit);
	}

	close(s);	

}

/*
 * Restore states for network interfaces
 */
static void
restore_netif()
{
	int s, i;
	struct vknetif_info *info;
	char tap_if[IFNAMSIZ];
	int tap_fd, tap_unit;
	
	s = socket(AF_INET, SOCK_DGRAM, 0);	/* for ioctl(SIOC) */
	if (s < 0)
		return;
	
	for (i = 0; i < NetifNum; ++i) {
		info = &NetifInfo[i];
		
		/* Open device file and bring up the tap interface */
		printf("[THAW] Opening tap%d interface.\n", info->tap_unit);
		bzero(tap_if, sizeof(tap_if));
		snprintf(tap_if, IFNAMSIZ, "tap%d", info->tap_unit); 
		tap_fd = netif_open_tap(tap_if, &tap_unit, s);
		if (tap_fd < 0) {
			printf("Failed to open tap%d interface\n", info->tap_unit);
			continue;
		}

		if (info->is_bridged) {
			printf("[THAW] Adding tap%d to %s\n", info->tap_unit, info->tap_bridge); 
			if (netif_add_tap2brg(info->tap_unit, info->tap_bridge, s) < 0) {
				printf("[THAW] Failed to add tap%d to %s\n", info->tap_unit, info->tap_bridge); 
				continue;
			}
		} else {
			if (netif_set_tapaddr(tap_unit, info->tap_addr, info->tap_mask, s) < 0) {
				printf("[THAW] Failed to set address for tap%d\n", info->tap_unit); 
				continue;
			}		
		}

		info->tap_fd = tap_fd;

		//XXX reset interfaces UP
		printf("[THAW] Restored interface tap%d\n\n", info->tap_unit);
	}
	
	close(s);
}

/*
 * XXX Find a way to restore the MAC address and baudrate
 * from the newly open tap (these can change between checkpoint
 * and restore e.g. if we restore the vkernel on another machine)
 * without a complete deattach from ethernet (maybe some ioctl??)
 */
static void 
netif_down(struct ifnet *ifp)
{
	/* Set the vke interface down */
	if (ifp->if_flags & IFF_UP) {
		printf("[CKPT] Setting %s down.\n", ifp->if_xname);
		
		if (ifp->if_flags & IFF_SMART) {
			/* Smart drivers twiddle their own routes */
		} else {
			crit_enter();
			if_down(ifp);
			crit_exit();	
		}
		if (ifp->if_ioctl) {
			ifnet_serialize_all(ifp);
			ifp->if_ioctl(ifp, SIOCSIFFLAGS, NULL, NULL);
			ifnet_deserialize_all(ifp);
		}
		//getmicrotime(&ifp->if_lastchange);
	}

	/* Detach Ethernet interface
	 * We have to detach it in order to 
	 * be restored with the new tap's mtu and MAC address */ 
	//ether_ifdetach(ifp);
}

static int
netif_del_tapbrg(int unit, char *bridge, int s)
{
	struct ifdrv ifd;
	struct ifbreq ifbr;

	bzero(&ifbr, sizeof(ifbr));
	snprintf(ifbr.ifbr_ifsname, sizeof(ifbr.ifbr_ifsname),
			"tap%d", unit);

	bzero(&ifd, sizeof(ifd));
	strlcpy(ifd.ifd_name, bridge, sizeof(ifd.ifd_name));
	ifd.ifd_cmd = BRDGDEL;
	ifd.ifd_len = sizeof(ifbr);
	ifd.ifd_data = &ifbr;

	if (ioctl(s, SIOCSDRVSPEC, &ifd) < 0) {
		//xxx ignore errno not exists?
		return -1;
	}

	return 0;
}

static struct ifnet *
lookup_ifnet(int unit)
{
	struct ifnet *ifp;
	char	xname[IFNAMSIZ];

	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		ksnprintf(xname, IFNAMSIZ, "%s%d", "vke", unit);

		if (strncmp(ifp->if_xname, xname, IFNAMSIZ) == 0)
			return ifp;
	}
		
	return NULL;
}

#endif
