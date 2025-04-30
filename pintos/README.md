# Pintos Lab 01: User Program
## Project description
### Part I: Trace Code 
Trace how argument passing work in OS, and explain how this work in the beginning of execution, including:
* Overview of function flow with key parameter setting.
* A diagram or structured description of stack layout after argument passing.
* Code snippets (if needed) to support your explanation.

### Part II: Implementation
* Print process termination messages whenever a user terminates, show the full name bypass to process_execute() and exit status. It should be formatted as: `printf ("%s: exit(%d)\n", ”);`
* Implement following 12 system calls in [userprog/syscall.c]:
    * Process Management: exit(), exec(), wait()
    * File I/O Management : create(), remove(), open(), filesize(), read(), write(), seek(), tell(), close()

## User Guide
### How to start the project?
You can simply search 'lab01' to check where should you start your project.    
At this stage, you should not modify any files in `filesys/`. We will be using the original file system implementation. However, you may need to call some functions provided in `file.c` to complete your system call implementation.

### Test the program  
* make your code under `pintos/userprog/`  
* test your program in `build/`
    * `make check`: run all tests and print pass/fail message for each.
    * `make check VERBOSE=1`: show progress of each test.

---

# Pintos Lab 03: Virtual memory
## Background
### What we will implement virtual memory on Pintos
Page table management
Swap in/out page
Stack growth

##　Project overview
## Resource Management Overview
You will need to design the following data structures:
* Supplemental page table:    
Enables page fault handling by supplementing the hardware page table with additional data about each page, because of the limitations imposed by the page table's format.
* Frame table    
When none is free, a frame must be made free by evicting some page from its frame.
* Swap table    
When swapping, picking an unused swap slot for evicting a page from its frame to the swap partition, and allow freeing a swap slot when its page is read back or the process whose page was swapped is terminated.

### Task 1: Paging
* All of these pages should be loaded lazily, that is, only as the kernel intercepts page faults for them.
* Upon eviction:    
Pages modified since load should be written to swap.
Unmodified pages, including read-only pages, should never be written to swap because they can always be read back from the executable.
* Implement a global page replacement algorithm that approximates LRU.    
Your algorithm should perform at least as well as the simple variant of the "second chance" or "clock" algorithm.

### Task 2: Accessing User Memory
* Adjust user memory access code in system call handling to deal with potential page faults.
* While accessing user memory, your kernel must either be prepared to handle such page faults, or it must prevent them from occurring.

###　Task 3: Stack Growth
* Purpose: Allows automatic stack growth during execution
* Allocate additional pages only if they "appear" to be stack accesses. Devise a heuristic that attempts to distinguish stack accesses from other accesses.

