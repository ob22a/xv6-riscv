#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  int on;

  if(argc != 2){
    fprintf(2, "usage: memtrace <0|1>\n");
    exit(1);
  }

  on = atoi(argv[1]);
  if(on != 0 && on != 1){
    fprintf(2, "memtrace: expected 0 or 1\n");
    exit(1);
  }

  if(memtrace(on) < 0){
    fprintf(2, "memtrace: syscall failed\n");
    exit(1);
  }
  printf("memtrace: %s\n", on ? "enabled" : "disabled");
  exit(0);
}
