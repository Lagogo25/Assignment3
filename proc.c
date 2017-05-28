#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);
extern int free_pages_bound;  // ours
extern int occupied_pages;    // ours
static void wakeup1(void *chan);


// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va. If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table 
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  #if defined (FIFO) || defined (SCFIFO) || defined (LAP)    
  p->plist.pgflt_counter = 0;
  p->plist.pgout_counter = 0;
  struct paging_meta_data *curr;
  for(curr = p->plist.frames; curr < &p->plist.frames[MAX_TOTAL_PAGES]; curr++){
    curr->pte = 0;
    curr->va = 0;
    curr->used = 0;
    curr->swapFile_offset = -1;
  }
  int i;
  for(i = 0; i < MAX_PSYC_PAGES; i++){
    p->plist.used_file[i] = 0;
  }
  #endif

  return p;
}


//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;
  #if defined (FIFO) || defined (SCFIFO) || defined (LAP)    
  if (np->pid > 2) // create swap file only for whoever is not init/shell
    createSwapFile(np);
  #endif
  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));
 
  pid = np->pid;
  #if defined (FIFO) || defined (SCFIFO) || defined (LAP)    
  if(proc->pid > 2){
    np->plist.pgflt_counter = proc->plist.pgflt_counter;
    np->plist.pgout_counter = proc->plist.pgout_counter;
    for(i=0; i<15; i++){
      np->plist.used_file[i] = proc->plist.used_file[i];
    }
    for(i=0; i<MAX_TOTAL_PAGES; i++){
      //saving its paging meta data  
      np->plist.frames[i].swapFile_offset = proc->plist.frames[i].swapFile_offset;
      np->plist.frames[i].age = proc->plist.frames[i].age;
      np->plist.frames[i].creation_time = proc->plist.frames[i].creation_time;
      np->plist.frames[i].va = proc->plist.frames[i].va;
      np->plist.frames[i].pte = walkpgdir(np->pgdir,(char*)PGROUNDDOWN(np->plist.frames[i].va), 0);
      np->plist.frames[i].used = proc->plist.frames[i].used;
      //reading the data of the current page from swapFile
      int k = 0;
      for(;k < PGSIZE; k+=128){
        char buffer[128];
        memset(buffer,0,128);
        int num_read = 1;
        num_read = readFromSwapFile(proc, buffer , proc->plist.frames[i].swapFile_offset*PGSIZE + k, 128);
        //writing the data to the new child process' swapFile
        if(num_read < 1)
          break;
        if(!writeToSwapFile(np, buffer, np->plist.frames[i].swapFile_offset*PGSIZE + k, num_read)){
          panic("problem with writing to swapFile\n");
        }
      }
    }
  }
  #endif
  // lock to force the compiler to emit the np->state write last.
  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);
  
  return pid;
}

int
counts_phys_memory(struct proc* p){
  int i;
  int counter=0;
  pte_t* pte;
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(p->plist.frames[i].va != 0 && p->plist.frames[i].swapFile_offset == -1){
      pte = walkpgdir(p->pgdir, (char*)PGROUNDDOWN(p->plist.frames[i].va), 0);
      if(*pte & PTE_P){
        counter++;
    }
  }
    
  }
return counter;
}

int
counts_file_memory(struct proc* p){
  int i;
  int counter=0;
  pte_t* pte;
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(p->plist.frames[i].va != 0 && p->plist.frames[i].swapFile_offset != -1){
       pte = walkpgdir(p->pgdir, (char*)PGROUNDDOWN(p->plist.frames[i].va), 0);
      if(*pte & PTE_PG){
        counter++;
      }
    }
  }
return counter;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  #ifdef TRUE // ??
  #if defined (FIFO) || defined (SCFIFO) || defined (LAP)    
  int counter_phys_memory = counts_phys_memory(proc);
  int counter_file_memory = counts_file_memory(proc);
  cprintf("%d %s %d %d %d %d %s \n", proc->pid, "ZOMBIE", counter_phys_memory + counter_file_memory, counter_file_memory, proc->plist.pgflt_counter, proc->plist.pgout_counter, proc->name);
  #endif
  cprintf("%d% free pages in the system \n", (free_pages_bound-occupied_pages)*100/free_pages_bound);
  #endif

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc child;      // will be the process child we are waiting for
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        child.pid = p->pid;           // whatever neccesary to remove it's swap
        child.swapFile = p->swapFile;
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        removeSwapFile(&child); // remove child's swap!
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot 
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    int counter_phys_memory = 0;
    int counter_file_memory = 0;
    pte_t* pte;
    for(i=0; i<MAX_TOTAL_PAGES; i++){
      if (p->plist.frames[i].va != 0){
        pte = walkpgdir(p->pgdir, (char*)PGROUNDDOWN(p->plist.frames[i].va), 0);
        if (p->plist.frames[i].swapFile_offset == -1){
          if(*pte & PTE_P){
            counter_phys_memory++;
          }
        }
        else if(*pte & PTE_PG){
          counter_file_memory++;
        }
      }
    }
    cprintf("%d %s %d %d %d %d %s", p->pid, state, counter_phys_memory + counter_file_memory, counter_file_memory, p->plist.pgflt_counter, p->plist.pgout_counter, p->name);

    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
  cprintf("%d% free pages in the system \n", (free_pages_bound-occupied_pages)*100/free_pages_bound);
}

//adding to pages list new page, returns 1 if success and 0 otherwise
int
add_page(struct proc* p, uint* pte, uint va, int offset){
  struct paging_meta_data *curr;
  
  for(curr = &p->plist.frames[0]; curr < &p->plist.frames[MAX_TOTAL_PAGES] ; curr++){
    if(offset == -1 && curr->used == 0){
      curr->pte = pte;
      curr->va = va;
      curr->used = 1;
      curr->swapFile_offset = -1;
      curr->creation_time = ticks;
      curr->age = 0;
      return 1;
    } else if (offset != -1 && curr->va == va){
        curr->swapFile_offset = offset;
        p->plist.used_file[offset] = 1;
        return 1;
    } 

  }
  return 0;
}

int
remove_page(struct proc* p, uint va){
  struct paging_meta_data *curr;
  for(curr = &p->plist.frames[0]; curr < &p->plist.frames[MAX_TOTAL_PAGES] ; curr++){
    if(curr->va == va){
      curr->used = 0;
      if(curr->swapFile_offset == -1){
        curr->pte = 0;
        return 1;
      }
      else{
        p->plist.used_file[curr->swapFile_offset] = 0;
        curr->swapFile_offset = -1;
        curr->pte = 0;
        return 1;
      }
    }
  }
  return 0;
}

void
print_pages(void){
  int i;
  cprintf("\n");
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(proc->plist.frames[i].used == 1){
      if(proc->plist.frames[i].swapFile_offset == -1)
        cprintf("process %d's page[%d] address is 0x%x , in the physical memory\n", proc->pid, i, proc->plist.frames[i].va);
      else
        cprintf("process %d's page[%d] address is 0x%x in %d offset in swapFile\n", proc->pid, i, proc->plist.frames[i].va, proc->plist.frames[i].swapFile_offset);
      cprintf("\n");
    }
  }
}


void
update_time(void){
  int i;
  for(i=0; i<MAX_TOTAL_PAGES; i++){
      if(proc->plist.frames[i].used == 1 && proc->plist.frames[i].swapFile_offset == -1){
        proc->plist.frames[i].age = proc->plist.frames[i].age >> 1;
        if(*proc->plist.frames[i].pte & PTE_A){
          proc->plist.frames[i].age = proc->plist.frames[i].age + BIT31;
        }
        *proc->plist.frames[i].pte = *proc->plist.frames[i].pte & ~PTE_A;
      }
  }
}


