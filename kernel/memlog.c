#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

#define MEMLOG_CAPACITY 32768

static struct {
  struct spinlock lock;
  struct spinlock read_lock;
  int initialized;
  int enabled;
  char buf[MEMLOG_CAPACITY];
  int head; // next write
  int len;  // number of valid bytes
} memlog;

static void
memlog_putc(char c)
{
  memlog.buf[memlog.head] = c;
  memlog.head = (memlog.head + 1) % MEMLOG_CAPACITY;
  if(memlog.len < MEMLOG_CAPACITY){
    memlog.len++;
  }
}

static void
memlog_puts(const char *s)
{
  while(*s){
    memlog_putc(*s++);
  }
}

static void
memlog_putuint(uint64 x)
{
  char tmp[21];
  int i = 0;

  if(x == 0){
    memlog_putc('0');
    return;
  }

  while(x > 0 && i < (int)sizeof(tmp)){
    tmp[i++] = '0' + (x % 10);
    x /= 10;
  }
  while(i > 0){
    memlog_putc(tmp[--i]);
  }
}

static void
memlog_puthex(uint64 x)
{
  char tmp[16];
  int i = 0;

  if(x == 0){
    memlog_puts("0x0");
    return;
  }

  while(x > 0 && i < (int)sizeof(tmp)){
    int d = x & 0xF;
    tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
    x >>= 4;
  }

  memlog_puts("0x");
  while(i > 0){
    memlog_putc(tmp[--i]);
  }
}

void
memlog_init(void)
{
  initlock(&memlog.lock, "memlog");
  initlock(&memlog.read_lock, "memlog_read");
  memlog.initialized = 1;
  memlog.enabled = 1;
  memlog.head = 0;
  memlog.len = 0;
}

void
memlog_enable(int on)
{
  if(!memlog.initialized)
    return;

  acquire(&memlog.lock);
  memlog.enabled = on ? 1 : 0;
  release(&memlog.lock);
}

int
memlog_is_enabled(void)
{
  int enabled = 0;
  if(!memlog.initialized)
    return 0;

  acquire(&memlog.lock);
  enabled = memlog.enabled;
  release(&memlog.lock);
  return enabled;
}

void
memlog_log_alloc(int pid, uint64 va, int pages_used, const char *source)
{
  if(!memlog.initialized)
    return;

  acquire(&memlog.lock);
  if(memlog.enabled){
    memlog_puts("ALLOC ");
    memlog_puts(source);
    memlog_puts(" pid=");
    memlog_putuint((uint64)pid);
    memlog_puts(" va=");
    memlog_puthex(va);
    memlog_puts(" pages=");
    memlog_putuint((uint64)pages_used);
    memlog_putc('\n');
  }
  release(&memlog.lock);
}

void
memlog_log_free(int pid, uint64 va_start, uint64 va_end, int pages_used)
{
  if(!memlog.initialized)
    return;

  acquire(&memlog.lock);
  if(memlog.enabled){
    memlog_puts("FREE pid=");
    memlog_putuint((uint64)pid);
    memlog_puts(" range=[");
    memlog_puthex(va_start);
    memlog_puts(",");
    memlog_puthex(va_end);
    memlog_puts(") pages=");
    memlog_putuint((uint64)pages_used);
    memlog_putc('\n');
  }
  release(&memlog.lock);
}

void
memlog_log_fifo_evict(int pid, uint64 va, uint64 seq)
{
  if(!memlog.initialized)
    return;

  acquire(&memlog.lock);
  if(memlog.enabled){
    memlog_puts("FIFO_EVICT pid=");
    memlog_putuint((uint64)pid);
    memlog_puts(" va=");
    memlog_puthex(va);
    memlog_puts(" seq=");
    memlog_putuint(seq);
    memlog_putc('\n');
  }
  release(&memlog.lock);
}

int
memlog_read_user(uint64 dst, int max, int clear_after_read)
{
  int i;
  int n;
  int start;
  struct proc *p = myproc();
  static char snapshot[MEMLOG_CAPACITY];

  if(!memlog.initialized || max <= 0)
    return 0;

  acquire(&memlog.read_lock);
  acquire(&memlog.lock);
  n = memlog.len;
  if(n > max)
    n = max;

  start = memlog.head - memlog.len;
  if(start < 0)
    start += MEMLOG_CAPACITY;

  for(i = 0; i < n; i++)
    snapshot[i] = memlog.buf[(start + i) % MEMLOG_CAPACITY];

  if(clear_after_read){
    memlog.head = 0;
    memlog.len = 0;
  }
  release(&memlog.lock);

  for(i = 0; i < n; i++){
    if(copyout(p->pagetable, dst + i, &snapshot[i], 1) < 0){
      release(&memlog.read_lock);
      return -1;
    }
  }
  release(&memlog.read_lock);
  return n;
}
