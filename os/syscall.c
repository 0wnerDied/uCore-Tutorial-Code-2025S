#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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
uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}
#else
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
#endif

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

/*
 * sys_spawn - Create a new process and execute a program.
 * @va: User-space virtual address of the filename to execute.
 *
 * Creates a new process in a single step, avoiding the overhead of
 * fork-then-exec. This is more efficient as it does not copy the
 * parent's address space.
 *
 * Returns:
 * The new process's PID on success.
 * -1 on error (e.g., invalid filename, out of memory, or process limit reached).
 */
uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	struct proc *np;
	int id;

	/*
	 * Copy the filename from user space to kernel space.
	 */
	if (copyinstr(p->pagetable, name, va, MAX_STR_LEN) < 0)
		return -1;

	id = get_id_by_name(name);
	if (id < 0)
		return -1;

	if ((np = allocproc()) == 0)
		return -1;

	np->parent = p;

	if (loader(id, np) < 0) {
		freeproc(np);
		return -1;
	}

	// add_task(np);

	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	if (prio < 2)
		return -1;

	struct proc *p = curr_proc();

	p->priority = prio;
	p->pass = BIG_STRIDE / p->priority;

	return prio;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd, uint64 stat)
{
	//TODO: your job is to complete the syscall
	return -1;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath,
	       uint64 flags)
{
	//TODO: your job is to complete the syscall
	return -1;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	//TODO: your job is to complete the syscall
	return -1;
}

uint64 sys_sbrk(int n)
{
	uint64 addr;
	struct proc *p = curr_proc();
	addr = p->program_brk;
	if (growproc(n) < 0)
		return -1;
	return addr;
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
		if (pte != 0 && (*pte & PTE_V))
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
#endif // #if 0

	uint64 end_page = end / PGSIZE;
	if (end_page > p->max_page)
		p->max_page = end_page;

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
		if (pte == 0 || (*pte & PTE_V) == 0)
			return -1;
	}

	uvmunmap(p->pagetable, start, (end - start) / PGSIZE, 1);

	for (uint64 pg = p->max_page; pg > 0; pg--) {
		uint64 va  = (pg - 1) * PGSIZE;
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte && (*pte & PTE_V)) {
			p->max_page = pg;
			return 0;
		}
	}
	p->max_page = 0;

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
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
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
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
		ret = sys_fstat(args[0], args[1]);
		break;
	case SYS_linkat:
		ret = sys_linkat(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_unlinkat:
		ret = sys_unlinkat(args[0], args[1], args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	case SYS_sbrk:
		ret = sys_sbrk(args[0]);
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
