// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line
extern pde_t *kern_pgdir;

struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Backtrace current function callstack", mon_backtrace },
	{ "clear", "Clear the console", mon_clear},
	{ "rainbow", "Test the colored console", mon_rainbow},
	{ "cpuid", "CPUID output in console", mon_cpuid},
	{ "showmap", "Show the mappings between given virtual memory range", mon_showmap },
    { "setperm", "Set the permission bits of a given mapping", mon_setperm },
    { "dumpmem", "Dump the content of a given virtual/physical memory range", mon_dumpmem},
	{ "step", "Single-steppedly execute the following instruction", mon_step},
	{ "s", "Single-steppedly execute the following instruction", mon_step},
	{ "continue", "continue execution", mon_continue},
	{ "c", "continue execution", mon_continue},
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;
	//highlight(1);
	for (i = 0; i < ARRAY_SIZE(commands); i++){
		set_fgcolor((i + 1) % COLOR_NUM);
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	}
	reset_fgcolor();
	//lightdown();
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf(F_blue B_white"Special kernel symbols:"ATTR_OFF"\n");
	set_fgcolor(COLOR_MAGENTA);
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("  kern_pgdir  %08x (virt)  %08x (phys)\n", kern_pgdir, UVPT);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	reset_fgcolor();
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	uint32_t ebp = read_ebp();
	set_fgcolor(COLOR_CYAN);
	set_bgcolor(COLOR_YELLOW);
	cprintf("Stack backtrace");
	reset_bgcolor();
	set_fgcolor(COLOR_RED);
	while (ebp){
		uint32_t eip = (uint32_t)*((int *)ebp + 1);
		cprintf("\nebp %x  eip %x  args", ebp, eip);
		int *args = (int *)ebp + 2;
		for (int i = 0; i < 4; i++)
			cprintf(" %08.x ", args[i]);
		cprintf("\n");
		struct Eipdebuginfo info;
		debuginfo_eip(eip, &info);
		cprintf("\t%s:%d: %.*s+%d\n",
			info.eip_file, info.eip_line,
			info.eip_fn_namelen, info.eip_fn_name, eip-info.eip_fn_addr);
		ebp = *(uint32_t *)ebp;
		//count++ ;
	}
	reset_fgcolor();
	return 0;
}

int
mon_clear(int argc, char **argv, struct Trapframe *tf)
{
	return clear();
}


void
rainbow(int stride)
{
	static const char msg[] = "rainbow!";
	for (int i = 0; i < COLOR_NUM; ++i) {
		set_fgcolor(i);
		set_bgcolor((i + stride) % COLOR_NUM);
		cprintf("%c", msg[i % (sizeof(msg) - 1)]);
	}
	reset_fgcolor();
	reset_bgcolor();
	cprintf("\n");
}
 
int 
mon_rainbow(int argc, char **argv, struct Trapframe *tf)
{
	for(int i = 1; i < COLOR_NUM; ++i)
		rainbow(i);
	return 0;
}

int
mon_cpuid(int argc, char **argv, struct Trapframe *tf)
{
	highlight(1);
	if (argc >= 2)
		print_cpuid(atoi(argv[1]));
	else
		print_cpuid(1);
	lightdown();
	return 0;
}

// map a capital character to permission bitnum
static inline uint32_t
char2perm(char c) {
	switch (c)
	{
		case 'G': return PTE_G;		//0x100
		case 'S': return PTE_PS;	//0x80
		case 'D': return PTE_D;		//0x40
		case 'A': return PTE_A;		//0x20
		case 'C': return PTE_PCD;	//0x10
		case 'T': return PTE_PWT;	//0x8
		case 'U': return PTE_U;		//0x4
		case 'W': return PTE_W;		//0x2
		case 'P': return PTE_P;		//0x1
		default: return -1;
	}
}

static const char* perm_string = "PWUTCADSG";

// permission number to a string of capital character
static inline void
perm2str(char* target, uint32_t perm) {
	if (perm >= 0x200) {
		warn("unexcepted permission!\n");
		perm -= 0x200;
	}

	int i;
	for (i = 8; i >= 0; i -= 1) {
		if (perm & (1 << i)) {
			*(target + 8 - i) = perm_string[i];
		}
		else
			*(target + 8 - i) = '-';
	}
}

// map a string of capital character to permission number
static inline uint32_t
str2perm(char* str){
	uint32_t perm = 0;
	while(*str)
		perm |= char2perm(*str++);
	if(perm < 0)
		panic("Wrong perm-character\n");
	// setting P bit should be forbidden
	perm &= ~char2perm('P');
	return perm;
}

#define PDE(pgdir,va) (pgdir[(PDX(va))])
#define PTE_PTR(pgdir,va) (((pte_t*) KADDR(PTE_ADDR(PDE(pgdir,va)))) + PTX(va))
#define PTE(pgdir,va) (*(PTE_PTR(pgdir,va)))
#define PERM(entry) (entry & 0xFFF)

int
mon_showmap(int argc, char **argv, struct Trapframe *tf)
{
	static const char *msg = 
    F_magenta"Usage: showmap <start> [<length>]\n"
	"Output: VA:[VA], PA:[PA], PERM-bit:[GSDACTUWP]"ATTR_OFF"\n";

	if (argc < 2 || ((argc == 2) && !isdigit(*argv[1])))
		goto help;

	uintptr_t vstart, vend;
    size_t vlen;
	pde_t pde;
    pte_t pte;

    vstart = (uintptr_t)strtol(argv[1], 0, 0);
    vlen = argc >= 3 ? (size_t)strtol(argv[2], 0, 0) : 1;
    vend = vstart + vlen;

    vstart = ROUNDDOWN(vstart, PGSIZE);
	cprintf(ATTR_bold);
    for(; vstart <= vend; ) {
		//cprintf("vstart: 0x%08x, vend: 0x%08x\n", vstart, vend);
		char permission[10] = { 0 };
		// permission[9] = '\0';
		pde = PDE((curenv?curenv->env_pgdir:kern_pgdir), vstart);
		if (pde & PTE_P) {
			if (pde & PTE_PS) {
				physaddr_t pvaddr = PTE_ADDR(pde) | (PTX(vstart) << PTXSHIFT);
				//cprintf("pde perm:0x%03x\n",PERM(pde));
				perm2str(permission, PERM(pde));
				cprintf("(PSE_ON) VA: 0x%08x, PA: 0x%08x, PERM: %s\n",
            		vstart, pvaddr, permission);
				vstart += LPGSIZE;
			}
			else {
				pte = PTE((curenv?curenv->env_pgdir:kern_pgdir), vstart);
				if (pte & PTE_P) {
					cprintf("pte perm:0x%03x\n", PERM(pte));
					perm2str(permission, PERM(pte));
					cprintf("(PSE_OFF) VA: 0x%08x, PA: 0x%08x, PERM: %s\n",
            			vstart, PTE_ADDR(pde), permission);
					vstart += PGSIZE;
				}
			}
        // pte = pgdir_walk(kern_pgdir, (void*)vstart, 0);
		// if (pte && *pte & PTE_P) {
        //     cprintf("VA: 0x%08x, PA: 0x%08x, U-bit: %d, W-bit: %d, PS-bt: %d\n",
        //     vstart, PTE_ADDR(*pte), !!(*pte & PTE_U), !!(*pte & PTE_W), !!(*pte & PTE_PS));
        } else {
            cprintf("VA: 0x%08x, PA: No Mapping\n", vstart);
			cprintf(ATTR_OFF);
			return 1;
		}
    }
	cprintf(ATTR_OFF);
    return 0;
help:
	cprintf(msg);
    return 1;
}

int 
mon_setperm(int argc, char **argv, struct Trapframe *tf) 
{
    static const char *msg = 
    F_magenta"Usage: setperm <virtual address> <permission>\n"
	"*For PSE-enabled pgd, PTE_PS will be auto-set."ATTR_OFF"\n";

    if (argc != 3)
        goto help;
    
    uintptr_t va;
    uint16_t perm;
	pde_t* pde;
    pte_t *pte;

    va = (uintptr_t)strtol(argv[1], 0, 0);
    perm = (uint16_t)strtol(argv[2], 0, 0);
	char permission[10];
	permission[9]='\0';
	strncpy(permission, argv[2], 9);
	perm = str2perm(permission);
	//cprintf("perm=0x%03x\n", perm);
	pde = curenv ? &curenv->env_pgdir[PDX(va)] : &kern_pgdir[PDX(va)];
	if (*pde & PTE_PS){
		if (*pde & PTE_P) {
			*pde = (*pde & ~0xFFF) | perm | PTE_P | PTE_PS;
			cprintf("New mapping = VA: 0x%08x, PA: 0x%08x, perm: 0x%03x.\n",
				va, PTE_ADDR(*pde)|(PTX(va) << PTXSHIFT)|PGOFF(va), PERM(*pde));
		}
		else {
			cprintf("No such mapping\n");
			return 1;
		}
	} else {
		pte = pgdir_walk((curenv?curenv->env_pgdir:kern_pgdir), (void*)va, 0);
		if (pte && *pte & PTE_P) {
			*pte = (*pte & ~0xFFF) | (perm & 0xFFF) | PTE_P;
			cprintf("New mapping = VA: 0x%08x, PA: 0x%08x, perm: 0x%03x.\n",
				va, PTE_ADDR(*pte)|PGOFF(va), PERM(*pte));
		} else {
			cprintf("No such mapping\n");
			return 1;
		}
	}
    return 0;
help: 
    cprintf(msg);
    return 1;
}

int
mon_dumpmem(int argc, char **argv, struct Trapframe *tf)
{
	static const char *msg =
    F_magenta"Usage: dumpmem [option] <start> <length>\n"
    "\t-p, --physical\tuse physical address\n"
	"\t[-v, --virtual]\tuse virtual address(default)"ATTR_OFF"\n";

	int recog = 0;
	int phys = 0;
	if (argc == 4) {
        int i;
        for (i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--physical")) {
				recog = 1;
                phys = 1;
                break;
            }
			else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--virtual")) {
				recog = 1;
                break;
            }
        }
        if (!recog)
            goto help;
        for (int j = i; j < argc - 1; ++j)
            argv[j] = argv[j + 1];
    } else if (argc != 3) {
        goto help;
    }

	uint32_t mstart, mend;
    size_t mlen;
    
    mstart = (uint32_t)strtol(argv[1], 0, 0);
    mlen = (size_t)strtol(argv[2], 0, 0);
    mend = mstart + mlen;

	cprintf(ATTR_bold);
    if (phys) {
        if (mend > (~KERNBASE + 1)) {
            cprintf("Target memory out of range\n"
			"Only dump to TOP.\n");
			mend = ~KERNBASE + 1;
        }
        for (; mstart < mend; ++mstart) {
            cprintf("[PA 0x%08x]: %02x\n", mstart, *(uint8_t*)KADDR(mstart));
        }
    } else {
        uint32_t next;
        pte_t *pte;
        while(mstart < mend) {
			if (PDE((curenv?curenv->env_pgdir:kern_pgdir), mstart) & PTE_PS) {
				if (PDE((curenv?curenv->env_pgdir:kern_pgdir), mstart) & PTE_P) {
					next = MIN((uint32_t)PGADDR(PDX(mstart), PTX(mstart) + 1, 0), mend);
					for (; mstart < next; ++mstart)
						cprintf("[VA 0x%08x, PA 0x%08x]: %02x\n",
						 mstart, PTE_ADDR(PDE((curenv?curenv->env_pgdir:kern_pgdir), mstart))
						 |(PTX(mstart) << PTXSHIFT)|PGOFF(mstart), *(uint8_t*)mstart);
				} else {
					cprintf("[VA 0x%08x, PA No-mapping]: None\n", mstart);

				}
			} else {
				if (!(pte = pgdir_walk((curenv?curenv->env_pgdir:kern_pgdir), (void*)mstart, 0))) {
					next = MIN((uint32_t)PGADDR(PDX(mstart) + 1, 0, 0), mend);
					for (; mstart < next; ++mstart)
						cprintf("[VA 0x%08x, PA No-mapping]: None\n", mstart);
				} else if (!(*pte & PTE_P)) {
					next = MIN((uint32_t)PGADDR(PDX(mstart), PTX(mstart) + 1, 0), mend);
					for (; mstart < next; ++mstart)
						cprintf("[VA 0x%08x, PA No-mapping]: None\n", mstart);
				} else {
					next = MIN((uint32_t)PGADDR(PDX(mstart), PTX(mstart) + 1, 0), mend);
					for (; mstart < next; ++mstart)
						cprintf("[VA 0x%08x, PA 0x%08x]: %02x\n", mstart, PTE_ADDR(*pte) | PGOFF(mstart), *(uint8_t*)mstart);
				}
			}
        }
    }
	cprintf(ATTR_OFF);
    return 0;
help:
	cprintf(msg);
	return 1;
}

int
mon_step(int argc, char **argv, struct Trapframe *tf)
{
	// if not user-source bp/db interrupt, do nothing
	if (!(tf && (tf->tf_trapno == T_DEBUG || tf->tf_trapno == T_BRKPT) && 
          ((tf->tf_cs & 3) == 3)))
        return 0;
    tf->tf_eflags |= FL_TF;
    return -1;
}

int
mon_continue(int argc, char **argv, struct Trapframe *tf)
{
	// if not user-source bp/db interrupt, do nothing
	if (!(tf && (tf->tf_trapno == T_DEBUG || tf->tf_trapno == T_BRKPT) && 
          ((tf->tf_cs & 3) == 3)))
        return 0;
    tf->tf_eflags &= ~FL_TF;
    return -1;
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
	reset_fgcolor();
	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
