#include "types.h"
#include "mmu.h"
#include "kernel.hh"
#include "amd64.h"
#include "cpu.hh"
#include "traps.h"
#include "queue.h"
#include "spinlock.h"
#include "condvar.h"
#include "proc.hh"
#include "kmtrace.hh"
#include "bits.hh"
#include "kalloc.hh"
#include "apic.hh"
#include "irq.hh"
#include "kstream.hh"
#include "hwvm.hh"
#include "refcache.hh"

extern "C" void __uaccess_end(void);

struct intdesc idt[256] __attribute__((aligned(16)));

static char fpu_initial_state[FXSAVE_BYTES];

// boot.S
extern u64 trapentry[];

static struct irq_info
{
  irq_handler *handlers;
} irq_info[256 - T_IRQ0];

u64
sysentry_c(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 num)
{
  if(myproc()->killed) {
    mtstart(trap, myproc());
    exit();
  }

  trapframe *tf = (trapframe*) (myproc()->kstack + KSTACKSIZE - sizeof(*tf));
  myproc()->tf = tf;
  u64 r = syscall(a0, a1, a2, a3, a4, a5, num);

  if(myproc()->killed) {
    mtstart(trap, myproc());
    exit();
  }

  return r;
}

int
do_pagefault(struct trapframe *tf)
{
  uptr addr = rcr2();
  if (myproc()->uaccess_) {
    if (addr >= USERTOP)
      panic("do_pagefault: %lx", addr);

    sti();
    if(pagefault(myproc()->vmap, addr, tf->err) >= 0){
#if MTRACE
      mtstop(myproc());
      if (myproc()->mtrace_stacks.curr >= 0)
        mtresume(myproc());
#endif
      return 0;
    }
    cprintf("pagefault: failed in kernel\n");
    tf->rax = -1;
    tf->rip = (u64)__uaccess_end;
    return 0;
  } else if (tf->err & FEC_U) {
      sti();
      if(pagefault(myproc()->vmap, addr, tf->err) >= 0){
#if MTRACE
        mtstop(myproc());
        if (myproc()->mtrace_stacks.curr >= 0)
          mtresume(myproc());
#endif
        return 0;
      }
      uerr.println("pagefault: failed in user for ", shex(addr),
                   " err ", (int)tf->err);
      cli();
  }
  return -1;
}

static inline void
lapiceoi()
{
  lapic->eoi();
}

void
trap(struct trapframe *tf)
{
  if (tf->trapno == T_NMI) {
    // The only locks that we can acquire during NMI are ones
    // we acquire only during NMI.
    if (sampintr(tf))
      return;
    panic("NMI");
  }

#if MTRACE
  if (myproc()->mtrace_stacks.curr >= 0)
    mtpause(myproc());
  mtstart(trap, myproc());
  // XXX mt_ascope ascope("trap:%d", tf->trapno);
#endif

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if (mycpu()->timer_printpc) {
      cprintf("cpu%d: proc %s rip %lx rsp %lx cs %x\n",
              mycpu()->id,
              myproc() ? myproc()->name : "(none)",
              tf->rip, tf->rsp, tf->cs);
      if (mycpu()->timer_printpc == 2 && tf->rbp > KBASE) {
        uptr pc[10];
        getcallerpcs((void *) tf->rbp, pc, NELEM(pc));
        for (int i = 0; i < 10 && pc[i]; i++)
          cprintf("cpu%d:   %lx\n", mycpu()->id, pc[i]);
      }
      mycpu()->timer_printpc = 0;
    }
    if (mycpu()->id == 0)
      timerintr();
    refcache::mycache->tick();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_COM2:
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%lx\n",
            mycpu()->id, tf->cs, tf->rip);
    // [Intel SDM 10.9 Spurious Interrupt] The spurious interrupt
    // vector handler should return without an EOI.
    //lapiceoi();
    break;
  case T_IRQ0 + IRQ_ERROR:
    cprintf("cpu%d: lapic error?\n", mycpu()->id);
    lapiceoi();
    break;
  case T_TLBFLUSH: {
    lapiceoi();
    mmu::shootdown::on_ipi();
    break;
  }
  case T_SAMPCONF:
    lapiceoi();
    sampconf();  
    break;
  case T_IPICALL: {
    extern void on_ipicall();
    lapiceoi();
    on_ipicall();
    break;
  }
  case T_DEVICE: {
    // Clear "task switched" flag to enable floating-point
    // instructions.  sched will set this again when it switches
    // tasks.
    clts();
    // Save current FPU state
    // XXX(Austin) This process could exit and free its fpu_state, but
    // scoped_gc_epoch breaks if I use it here.
    // XXX(Austin) Do I need to FWAIT first?
    struct proc *fpu_owner = mycpu()->fpu_owner;
    if (fpu_owner) {
      assert(fpu_owner->fpu_state);
      fxsave(fpu_owner->fpu_state);
    }
    // Lazily allocate myproc's FPU state
    if (!myproc()->fpu_state) {
      myproc()->fpu_state = kmalloc(FXSAVE_BYTES, "(fxsave)");
      if (!myproc()->fpu_state) {
        console.println("out of memory allocating fxsave region");
        myproc()->killed = 1;
        break;
      }
      memmove(myproc()->fpu_state, &fpu_initial_state, FXSAVE_BYTES);
    }
    // Restore myproc's FPU state
    fxrstor(myproc()->fpu_state);
    mycpu()->fpu_owner = myproc();
    break;
  }
  default:
    if (tf->trapno >= T_IRQ0 && irq_info[tf->trapno - T_IRQ0].handlers) {
      for (auto h = irq_info[tf->trapno - T_IRQ0].handlers; h; h = h->next)
        h->handle_irq();
      lapiceoi();
      piceoi();
      return;
    }

    if (tf->trapno == T_PGFLT && do_pagefault(tf) == 0)
      return;
      
    if (myproc() == 0 || (tf->cs&3) == 0)
      kerneltrap(tf);

    // In user space, assume process misbehaved.
    uerr.println("pid ", myproc()->pid, ' ', myproc()->name,
                 ": trap ", (u64)tf->trapno, " err ", (u32)tf->err,
                 " on cpu ", myid(), " rip ", shex(tf->rip),
                 " rsp ", shex(tf->rsp), " addr ", shex(rcr2()),
                 "--kill proc");
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running 
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == 0x3)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->get_state() == RUNNING && 
     (tf->trapno == T_IRQ0+IRQ_TIMER || myproc()->yield_)) {
    yield();
  }


  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == 0x3)
    exit();

#if MTRACE
  mtstop(myproc());
  if (myproc()->mtrace_stacks.curr >= 0)
    mtresume(myproc());
#endif
}

void
inittrap(void)
{
  u64 entry;
  u8 bits;
  int i;
  
  bits = INT_P | SEG_INTR64;  // present, interrupt gate
  for(i=0; i<256; i++) {
    entry = trapentry[i];
    idt[i] = INTDESC(KCSEG, entry, bits);
  }

  // Construct initial FPU state
  lcr0(rcr0() & ~(CR0_TS | CR0_EM));
  fninit();
  ldmxcsr(0x1f80);              // Mask all SSE exceptions
  fxsave(&fpu_initial_state);
}

void
initnmi(void)
{
  void *nmistackbase = ksalloc(slab_stack);
  mycpu()->ts.ist[1] = (u64) nmistackbase + KSTACKSIZE;

  if (mycpu()->id == 0)
    idt[T_NMI].ist = 1;
}

void
initseg(struct cpu *c)
{
  volatile struct desctr dtr;

  dtr.limit = sizeof(idt) - 1;
  dtr.base = (u64)idt;
  lidt((void *)&dtr.limit);

  // Load per-CPU GDT
  memmove(c->gdt, bootgdt, sizeof(bootgdt));
  dtr.limit = sizeof(c->gdt) - 1;
  dtr.base = (u64)c->gdt;
  lgdt((void *)&dtr.limit);

  // When executing a syscall instruction the CPU sets the SS selector
  // to (star >> 32) + 8 and the CS selector to (star >> 32).
  // When executing a sysret instruction the CPU sets the SS selector
  // to (star >> 48) + 8 and the CS selector to (star >> 48) + 16.
  u64 star = ((((u64)UCSEG|0x3) - 16)<<48)|((u64)KCSEG<<32);
  writemsr(MSR_STAR, star);
  writemsr(MSR_LSTAR, (u64)&sysentry);
  writemsr(MSR_SFMASK, FL_TF | FL_IF);
}

// Pushcli/popcli are like cli/sti except that they are matched:
// it takes two popcli to undo two pushcli.  Also, if interrupts
// are off, then pushcli, popcli leaves them off.
void
pushcli(void)
{
  u64 rflags;

  rflags = readrflags();
  cli();
  if(mycpu()->ncli++ == 0)
    mycpu()->intena = rflags & FL_IF;
}

void
popcli(void)
{
  if(readrflags()&FL_IF)
    panic("popcli - interruptible");
  if(--mycpu()->ncli < 0)
    panic("popcli");
  if(mycpu()->ncli == 0 && mycpu()->intena)
    sti();
}

// Record the current call stack in pcs[] by following the %rbp chain.
void
getcallerpcs(void *v, uptr pcs[], int n)
{
  uintptr_t rbp;
  int i;

  rbp = (uintptr_t)v;
  for(i = 0; i < n; i++){
    // Read saved %rip
    uintptr_t saved_rip;
    if (safe_read_vm(&saved_rip, rbp + sizeof(uintptr_t), sizeof(saved_rip)) !=
        sizeof(saved_rip))
      break;
    // Subtract 1 so it points to the call instruction
    pcs[i] = saved_rip - 1;
    // Read saved %rbp
    if (safe_read_vm(&rbp, rbp, sizeof(rbp)) != sizeof(rbp))
      break;
  }
  for(; i < n; i++)
    pcs[i] = 0;
}

void
irq::register_handler(irq_handler *handler)
{
  assert(valid());
  assert(vector == gsi + T_IRQ0);
  handler->next = irq_info[gsi].handlers;
  irq_info[gsi].handlers = handler;
}
