/*
 * Inserts a syscall, used to test
 * the vmspaces mappings from the vkernel
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/module.h>
#include <sys/sysent.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <sys/sysproto.h>
#include <sys/kern_syscall.h>
#include <sys/mman.h>
#include <sys/thread.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/vkernel.h>
#include <sys/vmspace.h>

#include <vm/vm_extern.h>
#include <vm/pmap.h>

#include <machine/vmparam.h>

#include <sys/sysref2.h>
#include <sys/mplock2.h>


static int
test_vm(struct proc *p, void *arg)
{
	struct vkernel_proc *vkp;
	struct proc *pp = curproc;

	vkp = pp->p_vkernel;
	if (vkp == NULL)
		return -1;

	get_mplock();
	lwkt_gettoken(&vkp->token);

	/* Test for vmspace mappings */
	struct vmspace_entry *vme;
	RB_FOREACH(vme, vmspace_rb_tree, &vkp->root) {
		vm_map_t map = &vme->vmspace->vm_map;
		vm_map_entry_t entry;

		kprintf("Printing vmspace %x info\n", (unsigned int)vme->id);
		kprintf("Start     End              Map Type  Flags     PDir\n");
		vm_map_lock_read(map);
		for (entry = map->header.next;
				(entry != &map->header);
				entry = entry->next) {

			if (entry->maptype != VM_MAPTYPE_NORMAL &&
					entry->maptype != VM_MAPTYPE_VPAGETABLE) {
				continue;
			}

			kprintf("0x%lx       0x%lx      %i         %i         0x%lx\n",
					(u_long)entry->start, (u_long)entry->end,
					entry->maptype, entry->eflags, entry->aux.master_pde);
		}
		vm_map_unlock_read(map);

	}
	lwkt_reltoken(&vkp->token);
	rel_mplock();

	return 0;
}


static struct sysent test_vmspaces_sysent = {
	0,
	test_vm
};

static int offset = NO_SYSCALL;

static int
load (struct module *module, int cmd, void *arg)
{
	int error = 0;

	switch (cmd){

		case MOD_LOAD:
			kprintf ("test_vmspaces loaded at %d\n", offset);
			break;

		case MOD_UNLOAD:
			kprintf ("test_vmspaces unloaded from %d\n", offset);
			break;

		default:
			error = EINVAL;
			break;
	}

	return 0;
}


SYSCALL_MODULE (test_vmspaces, &offset, &test_vmspaces_sysent, load, NULL);


