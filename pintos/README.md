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

