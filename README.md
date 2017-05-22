# Assignment3

proc.c:
in fork:
    if (proc != initproc)
        createSwapFile(np); // creates a swap file for proccess so he can use pages
in exit:
	removeSwapFile(proc); // delete the process swap file

mmu.h:
	#define MAX_PSYC_PAGES 15 //max process pages in RAM
	#define MAX_TOTAL_PAGES 30 //max process pages
	#define PTE_PG          0x200   // Page moved to secondary storage

proc.h:
	uint p_pages;
	char * pages_RAM[15];        // Data structe for pages in RAM
	char * pages_Disk[15];       // Data structe for pages in Disk

proc.c:
in growproc:
	p_pages=p_pages + n/PGSIZE;
	if(p_pages>=MAX_TOTAL_PAGES) return 0;

vm.c:
in allocuvm:
	if(proc->p_pages == MAX_PSYC_PAGES){
		change_page();
	}
	proc->pages_RAM[i]=a;
	
