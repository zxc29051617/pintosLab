#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include <devices/shutdown.h>

#include <string.h>
#include <filesys/file.h>
#include <devices/input.h>
#include <threads/malloc.h>
#include <threads/palloc.h>
#include "process.h"
#include "pagedir.h"
#include <threads/vaddr.h>
#include <filesys/filesys.h>

#define MAX_SYSCALL 20

// lab01 Hint - Here are the system calls you need to implement.

/* System call for process. */

void sys_halt(void);
void sys_exit(struct intr_frame* f);
void sys_exec(struct intr_frame* f);
void sys_wait(struct intr_frame* f);

/* System call for file. */
void sys_create(struct intr_frame* f);
void sys_remove(struct intr_frame* f);
void sys_open(struct intr_frame* f);
void sys_filesize(struct intr_frame* f);
void sys_read(struct intr_frame* f);
void sys_write(struct intr_frame* f);
void sys_seek(struct intr_frame* f);
void sys_tell(struct intr_frame* f);
void sys_close(struct intr_frame* f);


static void (*syscalls[MAX_SYSCALL])(struct intr_frame *) = {
  [SYS_HALT] = sys_halt,
  [SYS_EXIT] = sys_exit,
  [SYS_EXEC] = sys_exec,
  [SYS_WAIT] = sys_wait,
  [SYS_CREATE] = sys_create,
  [SYS_REMOVE] = sys_remove,
  [SYS_OPEN] = sys_open,
  [SYS_FILESIZE] = sys_filesize,
  [SYS_READ] = sys_read,
  [SYS_WRITE] = sys_write,
  [SYS_SEEK] = sys_seek,
  [SYS_TELL] = sys_tell,
  [SYS_CLOSE] = sys_close
};

static void syscall_handler (struct intr_frame *);
static void *check_ptr(const void *vaddr);
static int get_user(const uint8_t *uaddr);
static struct open_file *find_file(int fd);
void invalid_access (void);

void syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static int get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
     : "=&a" (result) : "m" (*uaddr));
  return result;
}

static void *check_ptr(const void *vaddr)
{
  if (vaddr == NULL || !is_user_vaddr(vaddr))
  invalid_access();

  for (size_t i = 0; i < sizeof(int); i++) {
    const void *addr = (uint8_t *)vaddr + i;
    if (!is_user_vaddr(addr) || !pagedir_get_page(thread_current()->pagedir, addr) || get_user(addr) == -1)
        invalid_access();
  }
}

static struct open_file *find_file(int fd)
{
  struct list *files = &thread_current()->files;
  for (struct list_elem *e = list_begin(files); e != list_end(files); e = list_next(e)) {
      struct open_file *f = list_entry(e, struct open_file, elem);
      if (f->fd == fd) {
          return f;
      }
  }
  return NULL;
}

void sys_exit(struct intr_frame* f)
{
  int *status_ptr = (int *)f->esp + 1;
  check_ptr(status_ptr);
  thread_current()->st_exit = *status_ptr;
  thread_exit();
}

void sys_exec(struct intr_frame* f)
{
  char **cmd_ptr = (char **)f->esp + 1;
  check_ptr(cmd_ptr);
  check_ptr(*cmd_ptr);
  f->eax = process_execute(*cmd_ptr); // return valid pid or -1
}

void sys_wait(struct intr_frame* f)
{
  uint32_t *args = f->esp;
  check_ptr(args + 1);
  int pid = args[1];
  f->eax = process_wait(pid);
}

void sys_write(struct intr_frame* f)
{
  uint32_t *args = (uint32_t)f->esp;

  int fd = args[1];
  const char *buffer = (const char *)args[2];
  off_t size = (off_t)args[3];

  check_ptr(buffer);
  check_ptr(buffer + size - 1);

  if(fd == 1){  // STDOUT
    putbuf(buffer, size);
    f->eax = size;
  }else{
    struct open_file *tmp = find_file(fd);
    if(tmp){
      acquire_file_lock();
      f->eax = file_write(tmp->file, buffer, size);
      release_file_lock();
    }else{
      f->eax = 0;
    }
  }
}

void sys_create(struct intr_frame* f)
{
  uint32_t *args = f->esp;
  const char *file = (const char *) args[1];
  unsigned initial_size = (unsigned)args[2];
  check_ptr (file);

  acquire_file_lock ();
  f->eax = filesys_create (file, initial_size);
  release_file_lock ();
}

void sys_remove(struct intr_frame* f)
{
  char **file_ptr = (char **)f->esp + 1;
  check_ptr(file_ptr);
  check_ptr(*file_ptr);
  acquire_file_lock ();
  f->eax = filesys_remove (*file_ptr);
  release_file_lock ();
}

void sys_open(struct intr_frame* f)
{
  char **file_ptr = (char **)f->esp + 1;
  check_ptr(file_ptr);
  check_ptr(*file_ptr);
  acquire_file_lock ();
  struct file *opened = filesys_open(*file_ptr);
  release_file_lock ();
  if(opened)
  {
    struct thread *t = thread_current();
    struct open_file *tmp = malloc(sizeof(struct open_file));
    tmp->fd = t->file_fd++;
    tmp->file = opened;
    list_push_back(&t->files, &tmp->elem);
    f->eax = tmp->fd;
  }else
  {
    f->eax = -1;
  }
}

void sys_filesize (struct intr_frame* f){

  uint32_t *args = (uint32_t)f->esp;
  int fd = args[1];
  struct open_file * tmp = find_file(fd);
  if (tmp)
  {
    acquire_file_lock ();
    f->eax = file_length (tmp->file);
    release_file_lock ();
  } 
  else
  {
    f->eax = -1;
  }
}

void sys_read (struct intr_frame* f)
{
  uint32_t *args = (uint32_t)f->esp;

  int fd = args[1];
  uint8_t  *buffer = (uint8_t *)args[2];
  off_t size = (off_t)args[3];

  check_ptr(buffer);
  check_ptr(buffer + size);

  if(fd == 0)
  {
    for(int i = 0; i < size; i++)
      buffer[i] = input_getc();
    f->eax = size;
  }else
  {
    struct open_file *tmp = find_file(fd);
    if(tmp)
    {
      acquire_file_lock();
      f->eax = file_read(tmp->file, buffer, size);
      release_file_lock(); 
    }else
    {
      f->eax = -1;
    }
  }
}

void sys_seek(struct intr_frame* f)
{
  uint32_t *args = f->esp;

  int fd = args[1];
  unsigned position = args[2];
  struct open_file *tmp = find_file(fd);
  if (tmp)
  {
    acquire_file_lock ();
    file_seek (tmp->file, position);
    release_file_lock();
  }
}

void sys_tell (struct intr_frame* f)
{
  uint32_t *args = f->esp;
  check_ptr(args + 1);

  int fd = args[1];
  struct open_file *tmp = find_file(fd);
  if (tmp)
  {
    acquire_file_lock ();
    f->eax = file_tell (tmp->file);
    release_file_lock ();
  }else{
    f->eax = -1;
  }
}

void sys_close (struct intr_frame* f)
{
  uint32_t *args = (uint32_t)f->esp;
  int fd = args[1];
  struct open_file *tmp = find_file (fd);
  if (tmp)
  {
    acquire_file_lock ();
    file_close (tmp->file);
    release_file_lock ();

    list_remove (&tmp->elem);
    free (tmp);
  }
}




/* System Call: void halt (void)
    Terminates Pintos by calling shutdown_power_off() (declared in devices/shutdown.h). 
*/
void sys_halt(void)
{
  shutdown_power_off();
}



void invalid_access (void)
{
  thread_current()->st_exit = -1;
  thread_exit ();
}

static void syscall_handler (struct intr_frame *f UNUSED) 
{
  check_ptr((int *)f->esp + 1);

  int sys_code = *(int *)f->esp;
  if (sys_code < 0 || sys_code >= MAX_SYSCALL)
    invalid_access();


  syscalls[sys_code](f);
}
