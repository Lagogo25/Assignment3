#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

#define PGSIZE 4096
#define COUNT 20

char* m[COUNT];


void wait_loop(int time)
{
    int i;
    while (time > 0)
    {
        i++;
        if (i % 10 == 0)
            printf(1,".");
        time--;
    }
    printf(1,"\n");
}

void allocate_pages(int from,int to)
{
    int i;
    for (i = from; i < to; ++i)
    {
        m[i] = sbrk(PGSIZE);
        printf(1, "allocated page #%d at address: %x\n", i, m[i]);
    }

}

int main(int argc,char *argv[])
{
    int i,j,id=-1;
    // allocated first 15
    allocate_pages(0,15);
    printf(1,"fork #1\n");
    id = fork();
    if (id == 0) {
        // use first pages to test SCFIFO and FIFO
        m[0][160] = 25; // page 0 addr 3000
        m[3][160] = 25; // page 3 addr 6000
        sleep(50);
    } else {
        id = fork();
        if (id == 0) {
            sleep(300);
            j=0;
            while (j < 15) {
                m[j][160] = 34;
                j++;}
            j=0;
            sleep(100);
            while (j < 10000) {
                getpid();
                m[0][160] = 34;
                m[1][160] = 34;
                j++;
            }
        }
    }
    allocate_pages(15,20);
    // allocate 5 more pages


    // not let's try to read our first 5 pages
    for(i = 0;i<5;i++)
        printf(1,"%d m[%d][160]=%d\n",getpid(),i,m[i][160]);

    if (id == 0)
    {
        printf(1,"%d out\n",getpid());
        exit();
    }
    else
        wait();
    wait();


    // now we will fork proc with written swap and test aging
    printf(1,"fork #2\n");
    id = fork();
    /* id = 0; */
    if (id == 0) {
        j = 100;
        while (j > 0) {
            getpid();
            m[12][160] = 25; // page 3 addr 6000
            m[13][160] = 25;
            j--;
        }
        sleep(301);
    }
    allocate_pages(20,25);
    for(i = 10;i<15;i++)
        printf(1,"%d m[%d][160]=%d\n",getpid(),i,m[i][160]);

    if (id == 0 )
        exit();
    else
        wait();

    exit();
}
