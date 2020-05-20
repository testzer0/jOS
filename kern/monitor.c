// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>
#include <inc/textdefines.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display backtrace", mon_backtrace },
	{ "showmappings", "Shows mappings for the addresses in the specified range", show_mappings},
	{ "setperm", "Sets permissions for the specified phys/virt page", set_perms},
	{ "dump", "Dumps memory from START to END", dump_memory},
};

/***** Implementations of basic kernel monitor commands *****/

int 
show_mappings(int argc, char **argv, struct Trapframe *tf)
{
	if (argc != 3) {
		cprintf("Usage: showmappings 0xSTART 0xEND\n");
		return 0;
	}

	uint32_t start_addr = (uint32_t) ROUNDDOWN(strtol(argv[1], NULL, 16), PGSIZE);
	uint32_t end_addr = (uint32_t) ROUNDDOWN(strtol(argv[2], NULL, 16), PGSIZE);

	if (start_addr >= end_addr) {
		cprintf("Start >= End!\n");
		return 0;
	}

	cprintf("START\tEND\tPTE_P\tPTE_W\tPTE_U\n");
	while (start_addr < end_addr) {
		pte_t *pte = pgdir_walk(kern_pgdir, (void *)start_addr, 0);
		if (!pte) {
			cprintf("0x%x\t0x%x\t0\t0\t0\n", start_addr, start_addr+PGSIZE);
		}
		else {
			cprintf("0x%x\t0x%x\t%d\t\t%d\t\t%d\n", start_addr, start_addr+PGSIZE, (*pte & PTE_P)/PTE_P, (*pte & PTE_W)/PTE_W, (*pte & PTE_U)/PTE_U);
		}
		start_addr += PGSIZE;
	}

	return 0;
}

int 
set_perms(int argc, char **argv, struct Trapframe *tf) 
{
	if (argc != 4) {
		cprintf("Usage: setperm 0/1 [i.e. PHYS/VIRT] 0xaddr PERM\n");
		cprintf("Addr will be rounded down to PGSIZE\n");
		return 0;
	}

	unsigned int virt = strtol(argv[1], NULL, 0);
	uint32_t addr = strtol(argv[2], NULL, 16);
	uint32_t perm = strtol(argv[3], NULL, 0);
	if (!virt) {
		addr = ROUNDDOWN((uint32_t) KADDR((physaddr_t)addr), PGSIZE);
	}

	pte_t *pte = pgdir_walk(kern_pgdir, (void *)addr, 0);
	if (!pte) {
		cprintf("Not mapped yet\n");
		return 0;
	}

	*pte = PTE_P | perm;

	return 0;
}

int 
dump_memory(int argc, char **argv, struct Trapframe *tf) 
{
	if (argc != 4) {
		cprintf("Usage: dump [0|1] <PHYS/VIRT> 0xSTART 0xEND\n");
		return 0;
	}
	unsigned int virt = strtol(argv[1], NULL, 0);
	char *start = (char *)strtol(argv[2], NULL, 16);
	char *end = (char *)strtol(argv[3], NULL, 16);
	char *temp;

	if (!virt) {
		start = (char *)KADDR((physaddr_t)start);
		end = (char *)KADDR((physaddr_t)end);
	}

	pte_t *pte = pgdir_walk(kern_pgdir, (void *)ROUNDDOWN(start, PGSIZE), 0);
	if (!pte) {
		temp = ROUNDUP(start+1, PGSIZE), end;
		if (temp > end) {
			temp = end;
		}
		while (start < temp) {
			if ((unsigned int)(start) % 0x20 == 0) {
				cprintf("\n0x%x : ", start);
			}
			cprintf("0x%x ", 0);
			start += 0x8;
		}
	}

	while (start < end) {
		pte_t *pte = pgdir_walk(kern_pgdir, (void *)start, 0);
		if (pte) {
			temp = ROUNDUP(start+1, PGSIZE), end;
			if (temp > end) {
				temp = end;
			}
			while (start < temp) {
				if ((unsigned int)(start) % 0x20 == 0) {
					cprintf("\n0x%x : ", start);
				}
				cprintf("0x%x ", *start);
				start += 0x8;
			}
		} 
		else {
			temp = ROUNDUP(start+1, PGSIZE), end;
			if (temp > end) {
				temp = end;
			}
			while (start < temp) {
				if ((unsigned int)(start) % 0x20 == 0) {
					cprintf("\n0x%x : ", start);
				}
				cprintf("0x%x ", 0);
				start += 0x8;
			}
		}
	}

	cprintf("\n");
	return 0;
}

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	cprintf("Stack backtrace:\n");

	unsigned int *ebp = (unsigned int *)read_ebp();

	while(ebp) {
		cprintf("ebp %08x eip %08x args %08x %08x %08x %08x %08x\n", ebp, ebp[1], ebp[2], ebp[3], ebp[4], ebp[5], ebp[6]);

		struct Eipdebuginfo einfo;
		debuginfo_eip(ebp[1], &einfo);

		cprintf("\t%s:%d: %.*s+%d\n", einfo.eip_file, einfo.eip_line,einfo.eip_fn_namelen, einfo.eip_fn_name, ebp[1] - einfo.eip_fn_addr);
		ebp = (unsigned int *)*ebp;
	}

	return 0;
}



/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
