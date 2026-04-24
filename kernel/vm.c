#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

// FIFO simulation tracks resident pages in a small queue.
#define MAX_TRACKED_PAGES 50

struct fifo_node {
  int pid;
  uint64 va;
  uint64 seq;
  struct fifo_node *prev;
  struct fifo_node *next;
  struct fifo_node *free_next;
};

struct {
  struct spinlock lock;
  int initialized;
  struct fifo_node nodes[MAX_TRACKED_PAGES];
  struct fifo_node *free;
  struct fifo_node *head;
  struct fifo_node *tail;
  uint64 next_seq;
  int tracked_pages;
} fifo_state;

static void fifo_init_once(void);
static void fifo_remove_node(struct fifo_node *n);
static void fifo_evict_oldest_locked(void);
static void fifo_track_page(int pid, uint64 va);
static void fifo_remove_range(int pid, uint64 va_start, uint64 va_end);
static int fifo_tracked_pages(void);

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

static void
fifo_init_once(void)
{
  if(fifo_state.initialized)
    return;

  initlock(&fifo_state.lock, "fifo_sim");
  fifo_state.free = 0;
  fifo_state.head = 0;
  fifo_state.tail = 0;
  fifo_state.next_seq = 1;
  fifo_state.tracked_pages = 0;
  for(int i = 0; i < MAX_TRACKED_PAGES; i++){
    fifo_state.nodes[i].free_next = fifo_state.free;
    fifo_state.free = &fifo_state.nodes[i];
  }
  fifo_state.initialized = 1;
}

static void
fifo_remove_node(struct fifo_node *n)
{
  if(n->prev)
    n->prev->next = n->next;
  else
    fifo_state.head = n->next;

  if(n->next)
    n->next->prev = n->prev;
  else
    fifo_state.tail = n->prev;

  n->prev = 0;
  n->next = 0;
  n->free_next = fifo_state.free;
  fifo_state.free = n;
  if(fifo_state.tracked_pages > 0)
    fifo_state.tracked_pages--;
}

static void
fifo_evict_oldest_locked(void)
{
  struct fifo_node *victim = fifo_state.head;
  int before = fifo_state.tracked_pages;
  if(victim == 0)
    return;
  printf("EVICT: pid %d va %p seq %d tracked %d/%d\n",
    victim->pid, (void*)victim->va, (int)victim->seq, before, MAX_TRACKED_PAGES);
  fifo_remove_node(victim);
}

static void
fifo_track_page(int pid, uint64 va)
{
  struct fifo_node *n, *cur, *next;

  fifo_init_once();
  acquire(&fifo_state.lock);
  // Keep at most one queue node per (pid, va).
  for(cur = fifo_state.head; cur; cur = next){
    next = cur->next;
    if(cur->pid == pid && cur->va == va){
      fifo_remove_node(cur);
    }
  }
  if(fifo_state.free == 0){
    fifo_evict_oldest_locked();
  }
  if(fifo_state.free == 0){
    release(&fifo_state.lock);
    return;
  }

  n = fifo_state.free;
  fifo_state.free = n->free_next;
  n->free_next = 0;
  n->pid = pid;
  n->va = va;
  n->seq = fifo_state.next_seq++;
  n->prev = fifo_state.tail;
  n->next = 0;

  if(fifo_state.tail)
    fifo_state.tail->next = n;
  else
    fifo_state.head = n;
  fifo_state.tail = n;
  fifo_state.tracked_pages++;
  release(&fifo_state.lock);
}

static void
fifo_remove_range(int pid, uint64 va_start, uint64 va_end)
{
  struct fifo_node *cur, *next;

  fifo_init_once();
  acquire(&fifo_state.lock);
  for(cur = fifo_state.head; cur; cur = next){
    next = cur->next;
    if(cur->pid == pid && cur->va >= va_start && cur->va < va_end){
      fifo_remove_node(cur);
    }
  }
  release(&fifo_state.lock);
}

void
fifo_remove_pid(int pid)
{
  struct fifo_node *cur, *next;
  int removed = 0;

  fifo_init_once();
  acquire(&fifo_state.lock);
  for(cur = fifo_state.head; cur; cur = next){
    next = cur->next;
    if(cur->pid == pid){
      fifo_remove_node(cur);
      removed++;
    }
  }
  int remaining = fifo_state.tracked_pages;
  release(&fifo_state.lock);
  if(removed > 0){
    printf("FIFO_REMOVE_PID: pid %d removed %d tracked %d/%d\n",
      pid, removed, remaining, MAX_TRACKED_PAGES);
  }
}

static int
fifo_tracked_pages(void)
{
  int n;
  fifo_init_once();
  acquire(&fifo_state.lock);
  n = fifo_state.tracked_pages;
  release(&fifo_state.lock);
  return n;
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  fifo_init_once();
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  struct proc *p = myproc();
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }

    if(p != 0){
      p->pages_used++;
      fifo_track_page(p->pid, a);
      printf("TRACK: uvmalloc pid %d va %p pages %d tracked %d/%d\n",
        p->pid, (void*)a, p->pages_used, fifo_tracked_pages(), MAX_TRACKED_PAGES);
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  struct proc *p = myproc();
  uint64 va_start = PGROUNDUP(newsz);
  uint64 va_end = PGROUNDUP(oldsz);

  if(newsz >= oldsz)
    return oldsz;

  if(va_start < va_end){
    int npages = (va_end - va_start) / PGSIZE;
    uvmunmap(pagetable, va_start, npages, 1);
    if(p != 0){
      fifo_remove_range(p->pid, va_start, va_end);
    }
  }

  int pages_removed = (va_end - va_start) / PGSIZE;
  if(p != 0){
    p->pages_used -= pages_removed;

    if (p->pages_used < 0){
      panic("pages_used corrupted");
    }
    printf("FREE: pid %d range [%p,%p) pages %d tracked %d/%d\n",
      p->pid, (void*)va_start, (void*)va_end, p->pages_used, fifo_tracked_pages(), MAX_TRACKED_PAGES);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  p->pages_used++;
  fifo_track_page(p->pid, va);
  printf("TRACK: vmfault pid %d va %p pages %d tracked %d/%d\n",
    p->pid, (void*)va, p->pages_used, fifo_tracked_pages(), MAX_TRACKED_PAGES);
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}
