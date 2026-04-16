#include "kernel/types.h"
#include "user/user.h"

int main(){
    printf("Testing FIFO. Number of pages at the start: %d\n",getmemusage());
    for(int i = 0; i < 100; i++){
        sbrk(4096);
    }
    printf("Number of pages at the end: %d.\n",getmemusage());
    
    return 0;
}
