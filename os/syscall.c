#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

#if 0
uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	// YOUR CODE
	val->sec = 0;
	val->usec = 0;

	/* The code in `ch3` will leads to memory bugs*/

	// uint64 cycle = get_cycle();
	// val->sec = cycle / CPU_FREQ;
	// val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return 0;
}
#endif // #if 0

uint64 sys_gettimeofday(uint64 val_va, int _tz)
{
	struct proc *p = curr_proc();
	TimeVal val;
	uint64 cycle = get_cycle();

	val.sec = cycle / CPU_FREQ;
	val.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

	if (copyout(p->pagetable, val_va, (char *)&val,
			sizeof(TimeVal)) < 0)
		return -1;

	return 0;
}

uint64 sys_sbrk(int n)
{
	uint64 addr;
        struct proc *p = curr_proc();
        addr = p->program_brk;
        if(growproc(n) < 0)
                return -1;
        return addr;	
}



// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_trace here
*/
int sys_trace(int trace_request, uint64 id, uint8 data)
{
	struct proc *p = curr_proc();

	/*
	 * Handle syscall count query first to avoid unnecessary
	 * page table operations. In case 2, 'id' represents a
	 * syscall number, not a memory address, so
	 * calling walk() would fail and cause incorrect behavior.
	 */
	if (trace_request == 2)
		return (id < 512) ? p->syscall_cnt[id] : -1;

	pte_t *pte = walk(p->pagetable, id, 0);
	uint8 val;

#ifdef LAZY_ALLOCATION
	if (pte != 0 && (*pte & PTE_V) == 0 && (*pte & PTE_LAZY)) {
		if (handle_lazy_fault(p->pagetable, id) == 0) {
			pte = walk(p->pagetable, id, 0);
			if (pte == 0)
				goto err;
		}
	}
#endif

	if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
		goto err;

	switch (trace_request) {
	case 0:
		if ((*pte & PTE_R) == 0)
			goto err;

		if (copyin(p->pagetable, (char *)&val, id, 1) < 0)
			goto err;

		return (int)val;
	case 1:
		if ((*pte & PTE_W) == 0)
			goto err;

		if (copyout(p->pagetable, id, (char *)&data, 1) < 0)
			goto err;

		return 0;
	default:
err:
		return -1;
	}
}

int sys_mmap(uint64 start, uint64 len, int prot, int flags)
{
	struct proc *p = curr_proc();

	if (start % PGSIZE != 0)
		return -1;

	if ((prot & ~0x7) != 0)
		return -1;
	if ((prot & 0x7) == 0)
		return -1;

	if (len == 0)
		return 0;

	uint64 end = PGROUNDUP(start + len);
	for (uint64 va = start; va < end; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
#ifdef LAZY_ALLOCATION
		if (pte != 0 && ((*pte & PTE_V) || (*pte & PTE_LAZY)))
#else
		if (pte != 0 && (*pte & PTE_V))
#endif
			return -1;
	}
#if 0
	int xperm = 0;

	if (prot & 0x2)
		xperm |= PTE_W;
	if (prot & 0x4)
		xperm |= PTE_X;

	if (uvmalloc(p->pagetable, start, end, xperm) == 0)
		return -1;
#else // #if 0
#ifdef LAZY_ALLOCATION
	int pte_flags = PTE_U;

	if (prot & 0x1)
		pte_flags |= PTE_R;
	if (prot & 0x2)
		pte_flags |= PTE_W;
	if (prot & 0x4)
		pte_flags |= PTE_X;

	if (mappages(p->pagetable, start, end - start, 0, pte_flags) != 0)
		return -1;
#else // #ifdef LAZY_ALLOCATION
	/*
	 * Manual page allocation and mapping loop.
	 * Unlike uvmalloc(), this gives us precise control
	 * over permission bits to match the exact prot
	 * flags specified by the user. We allocate physical
	 * pages one by one, zero them for security,
	 * set only the requested permission bits (crucial
	 * for RISC-V compliance where PTE_W=1,PTE_R=0 is
	 * illegal), and install the mapping via mappages().
	 */
	for (uint64 va = start; va < end; va += PGSIZE) {
		char *pa = kalloc();
		int pte_flags = PTE_V | PTE_U;

		if (!pa)
			return -1;

		memset(pa, 0, PGSIZE);

		if (prot & 0x1)
			pte_flags |= PTE_R;
		if (prot & 0x2)
			pte_flags |= PTE_W;
		if (prot & 0x4)
			pte_flags |= PTE_X;

		if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, pte_flags) != 0) {
			kfree(pa);
			return -1;
		}
	}
#endif // #ifdef LAZY_ALLOCATION
#endif // #if 0

	return 0;
}

int sys_munmap(uint64 start, uint64 len)
{
	struct proc *p = curr_proc();

	if (start % PGSIZE != 0)
		return -1;

	if (len == 0)
		return 0;

	uint64 end = PGROUNDUP(start + len);
	for (uint64 va = start; va < end; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
#ifdef LAZY_ALLOCATION
		if (pte == 0 || ((*pte & PTE_V) == 0 && (*pte & PTE_LAZY) == 0))
#else
		if (pte == 0 || (*pte & PTE_V) == 0)
#endif
			return -1;
	}

	uvmunmap(p->pagetable, start, (end - start) / PGSIZE, 1);

	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter here
	*/
	struct proc *p = curr_proc();
	(id < 512) ? p->syscall_cnt[id]++ : 0;

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_sbrk:
		ret = sys_sbrk(args[0]);
		break;
	/*
	* LAB1: you may need to add SYS_trace case here
	*/
	case SYS_trace:
		ret = sys_trace(args[0], args[1], args[2]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
