#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define MEMLOG_READ_MAX 32768

static char logbuf[MEMLOG_READ_MAX + 1];

int
main(int argc, char **argv)
{
  int clear_after_read = 1;
  int fd;
  int n;

  if(argc > 1 && argv[1][0] == 'k'){
    clear_after_read = 0;
  }

  n = memlogread(logbuf, MEMLOG_READ_MAX, clear_after_read);
  if(n < 0){
    fprintf(2, "memlogdump: memlogread failed\n");
    exit(1);
  }

  logbuf[n] = 0;

  fd = open("memlog.txt", O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0){
    fprintf(2, "memlogdump: open memlog.txt failed\n");
    exit(1);
  }
  if(write(fd, logbuf, n) != n){
    fprintf(2, "memlogdump: write failed\n");
    close(fd);
    exit(1);
  }
  close(fd);

  printf("memlogdump: wrote %d bytes to memlog.txt\n", n);
  exit(0);
}
