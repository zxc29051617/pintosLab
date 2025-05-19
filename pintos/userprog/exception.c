#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/page.h"
#include "vm/frame.h"

#define MAX_STACK_SIZE 0x800000  // 8MB stack limit

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);
extern void sys_exit (int status);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      sys_exit(-1); // 終止 父線程不再等待

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      sys_exit(-1); // 終止 父線程不再等待
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV-ES:--" exception "Interrupt 14--Page Fault
     Exception (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* 核心訪問用戶地址 */
  if (!user && is_user_vaddr(fault_addr)) {
    /* 導致這種情況的是內核代碼嘗試訪問用戶內存，
       但是相應的page不存在或無效。*/
    void *esp = thread_current()->current_esp;
    if (esp == NULL) {
      esp = f->esp;
    }
    
    /* 如果這是堆疊訪問，嘗試增長堆疊 */
    if (fault_addr >= esp - 32 && fault_addr < PHYS_BASE) {
      /* 檢查堆疊大小限制 (8MB) */
      if (PHYS_BASE - pg_round_down(fault_addr) <= MAX_STACK_SIZE) {
        /* 嘗試分配新的堆疊page */
        if (vm_alloc_page(VM_STACK, pg_round_down(fault_addr), true)) {
          struct suppPage *page = spt_find_page(thread_current()->spt, pg_round_down(fault_addr));
          if (page && vm_do_claim_page(page)) {
            return; /* 成功處理page錯誤 */
          }
        }
      }
    }
    
    /* 如果不是堆疊訪問或堆疊增長失敗，則終止當前線程 */
    sys_exit(-1);
    return;
  }

  /* 檢查是否為無效訪問 */
  if (!is_user_vaddr(fault_addr) || !not_present) {
    sys_exit(-1);
    return;
  }

  /* 檢查是否為堆疊訪問 */
  bool stack_access = false;
  void *esp = user ? f->esp : thread_current()->current_esp;
  
  if (esp != NULL && fault_addr >= esp - 32 && fault_addr < PHYS_BASE) {
    stack_access = true;
  } else if (fault_addr >= PHYS_BASE - MAX_STACK_SIZE && fault_addr < PHYS_BASE) {
    /* 檢查是否為 PUSHA 指令 (0x60) 或其他堆疊操作 */
    uint8_t *eip = (uint8_t *)f->eip;
    if (user && eip && is_user_vaddr(eip) && pagedir_get_page(thread_current()->pagedir, eip)) {
      uint8_t opcode = *eip;
      if (opcode == 0x60 || // pusha
          opcode == 0x50 || // push eax
          opcode == 0x51 || // push ecx
          opcode == 0x52 || // push edx
          opcode == 0x53 || // push ebx
          opcode == 0x54 || // push esp
          opcode == 0x55 || // push ebp
          opcode == 0x56 || // push esi
          opcode == 0x57 || // push edi
          opcode == 0x89 || // mov
          opcode == 0x8b) { // mov
        stack_access = true;
      }
    }
  }
  
  if (stack_access) {
    /* 檢查堆疊大小限制 (8MB) */
    if (PHYS_BASE - pg_round_down(fault_addr) <= MAX_STACK_SIZE) {
      /* 嘗試分配新的堆疊page */
      if (vm_alloc_page(VM_STACK, pg_round_down(fault_addr), true)) {
        struct suppPage *page = spt_find_page(thread_current()->spt, pg_round_down(fault_addr));
        if (page && vm_do_claim_page(page)) {
          return; /* 成功處理page錯誤 */
        }
      }
    }
  }

  /* 嘗試從補充頁表中找到page */
  struct suppPage *page = spt_find_page(thread_current()->spt, pg_round_down(fault_addr));
  if (page == NULL) {
    sys_exit(-1);
    return;
  }

  /* 嘗試加載page */
  if (!vm_do_claim_page(page)) {
    sys_exit(-1);
    return;
  }
}

