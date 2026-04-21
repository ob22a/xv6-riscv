#include "kernel/types.h"
#include "user/user.h"

int main()
{
  printf("Pages used at the start not counting the program itself: %d\n", getmemusage());
  sbrk(4096); // Request 4096 byte of memory which is one page
  printf("Page used after 1 page addition: %d\n",getmemusage());
  sbrk(4096); // Request 4096 byte of memory which is one page
  printf("Page used after 2 pages addition: %d\n",getmemusage());

  sbrk(-4096); // shrinking by a page
  printf("Page used after shrinking 1 page: %d\n",getmemusage());

  char *lazy = sbrklazy(4096);
  printf("After lazy sbrk (before touch): %d\n", getmemusage());
  lazy[0] = 'x'; // force vmfault-backed allocation
  printf("After lazy page touch: %d\n", getmemusage());

  // Test while forking 
  int pid=fork();
  if(pid==0){
   printf("child: %d\n", getmemusage());
   exit(0);
  } 
  else {
    wait(0);
    printf("parent: %d\n", getmemusage());
  }
  exit(0);
}