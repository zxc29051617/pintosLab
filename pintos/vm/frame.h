#include "threads/thread.h"
#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include <stdbool.h>
#include "threads/palloc.h"
#include <hash.h>
#include "lib/kernel/hash.h"
#include "threads/synch.h"

/* Forward declarations ------------- */
struct suppPage;

//The frame table entry that contains a user page
struct frame {
    void *kva;                 /* 內核虛擬位址 (from palloc)      */
    struct suppPage *page;     /* 若已映射，指向對應 suppPage     */
    struct thread *owner;      /* 擁有該 pagedir 的執行緒         */
    struct list_elem elem;     /* 串到全域 frame_table            */
    bool pinned;               /* true ⇒ 不得被驅逐               */
};

/* 初始化 → 在 vm_init() 早期呼叫 */
void vm_frame_init (void);

/* 主要介面 */
struct frame *frame_allocate (enum palloc_flags flags);
void          frame_free     (struct frame *fr);

/* 允許外部快速 pin / unpin （I/O 前後）*/
void frame_pin   (struct frame *fr);
void frame_unpin (struct frame *fr);

#endif /* vm/frame.h */