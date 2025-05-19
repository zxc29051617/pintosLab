#include <hash.h>
#include <list.h>
#include <stdio.h>
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"

#include "vm/frame.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "vm/swap.h"
#include <debug.h> 

static struct list frame_table;       /* 雙向循環 list — 保存所有 frame */
static struct lock frame_lock;        /* 保護 frame_table + clock_hand */
static struct list_elem *clock_hand;  /* 指向下一個要檢查的 frame     */


/* 將 list_elem 轉回 struct frame* */
static inline struct frame *
elem_to_frame (struct list_elem *e)
{
    return list_entry (e, struct frame, elem);
}

/* Clock algorithm：找到「un-pinned & accessed=0」的 frame */
static struct frame *
select_victim (void)
{
    ASSERT (lock_held_by_current_thread (&frame_lock));
    
    size_t table_sz = list_size (&frame_table);
    if (table_sz == 0) return NULL;

    /* 從 clock_hand 開始繞，只要繞過的 elem 數 >= 表大小就保證看過全部 */
    for (size_t scanned = 0; scanned < table_sz * 2; scanned++)
    {
        if (clock_hand == list_end (&frame_table))
            clock_hand = list_begin (&frame_table);

        struct frame *fr = elem_to_frame (clock_hand);
        clock_hand = list_next (clock_hand);        /* hand 往前移 */

        if (fr->pinned)        /* 被pin住 => 跳過 */
            continue;

        /* accessed? 若被 access 就清 bit, 給第二次機會 */
        if (pagedir_is_accessed (fr->owner->pagedir, fr->page->va))
        {
            pagedir_set_accessed (fr->owner->pagedir, fr->page->va, false);
            continue;
        }
        /* un-pinned & accessed=0 → 正式選為 victim */
        return fr;
    }
    /* 全 table 皆 pinned，理論上不應發生 */
    return NULL;
}

/* 把 victim frame 驅逐出去（swap 或寫回檔案），並返回它的 kva */
static bool
evict_frame (struct frame *victim)
{
    ASSERT (lock_held_by_current_thread (&frame_lock));
    ASSERT (victim && victim->page);

    struct suppPage *page = victim->page;
    struct thread   *owner = victim->owner;

    /* 先在 pagedir 斷開映射，避免 race */
    pagedir_clear_page (owner->pagedir, page->va);

    /* 臨時釋放鎖，以便執行可能會休眠的操作 */
    lock_release (&frame_lock);

    bool ok = false;
    switch (page->type)
    {
      case VM_ANON:
      case VM_STACK:
        ok = swap_out (page);          /* 把內容寫到 swap slot */
        break;

      case VM_FILE:
        /* 若是 dirty 且可寫 -> write-back；這裡先偷懶忽略 write-back */
        if (pagedir_is_dirty (owner->pagedir, page->va) && page->writable)
            ; /* TODO: file write-back (mmap write) */
        page->in_swap = false;
        ok = true;
        break;

      default:
        NOT_REACHED ();
    }
    
    /* 重新獲取鎖 */
    lock_acquire (&frame_lock);
    
    if (ok)
        page->frame = NULL;            /* 斷聯繫，頁狀態已更新 */

    return ok;
}


/* 
 * 初始化 frame
 * 將會在 vm_init 中調用 
 */
void
vm_frame_init (void)
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = list_end (&frame_table);
}

/* 分配一塊 user frame；若分配失敗會嘗試驅逐一塊 frame */
struct frame *
vm_frame_allocate (enum palloc_flags flags, void *upage)
{
    ASSERT (flags & PAL_USER);

    void *kva = NULL;
    struct frame *victim = NULL;

    lock_acquire (&frame_lock);

    /* 1. 先直接向 palloc 要 */
    kva = palloc_get_page (flags);
    if (kva == NULL)
    {
        /* 2. OOM → 找 victim 驅逐 */
        victim = select_victim ();
        if (victim == NULL)
        {
            lock_release (&frame_lock);
            return NULL;  // 無法找到可驅逐的page
        }
        
        /* 保存 victim 的 kva，以便稍後使用 */
        kva = victim->kva;
        
        /* evict_frame 會臨時釋放鎖 */
        if (!evict_frame (victim))
        {
            lock_release (&frame_lock);
            return NULL;  // swap失敗
        }
        
        /* 驅逐成功，從 frame_table 中移除並釋放 victim 結構 */
        list_remove (&victim->elem);
        free (victim);
    }

    /* 3. 建立新 frame 結構 */
    struct frame *fr = malloc (sizeof *fr);
    if (fr == NULL)
    {
        lock_release (&frame_lock);
        palloc_free_page (kva);  // 釋放剛分配的page
        return NULL;
    }

    fr->kva   = kva;
    fr->page  = NULL;         /* 在 page.c 中設置 */
    fr->owner = thread_current ();
    fr->pinned = false;

    list_push_back (&frame_table, &fr->elem);

    lock_release (&frame_lock);
    return fr;
}

void
vm_frame_free (void *kva)
{
    if (kva == NULL) return;

    struct frame *fr = NULL;

    lock_acquire (&frame_lock);
    
    // 查找要釋放的frame
    struct list_elem *e;
    
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame *f = elem_to_frame (e);
        if (f->kva == kva) 
        {
            fr = f;
            list_remove (&f->elem);
            break;
        }
    }
    
    lock_release (&frame_lock);
    
    // 在鎖外釋放資源，避免在持有鎖時調用 palloc_free_page
    if (fr != NULL)
    {
        palloc_free_page (fr->kva);
        free (fr);
    }
}


void
vm_frame_pin (void *kva)
{
    if (kva == NULL) return;
    
    /* 檢查當前線程是否已經持有鎖，如果是則直接操作 */
    bool already_holding_lock = lock_held_by_current_thread(&frame_lock);
    
    if (!already_holding_lock)
        lock_acquire (&frame_lock);
    
    /* 查找要 pin 的frame */
    struct list_elem *e;
    
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame *fr = elem_to_frame (e);
        if (fr->kva == kva) 
        {
            fr->pinned = true;
            break;
        }
    }
    
    if (!already_holding_lock)
        lock_release (&frame_lock);
}

void
vm_frame_unpin (void *kva)
{
    if (kva == NULL) return;
    
    /* 檢查當前線程是否已經持有鎖，如果是則直接操作 */
    bool already_holding_lock = lock_held_by_current_thread(&frame_lock);
    
    if (!already_holding_lock)
        lock_acquire (&frame_lock);
    
    /* 查找要 unpin 的frame */
    struct list_elem *e;
    
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame *fr = elem_to_frame (e);
        if (fr->kva == kva) 
        {
            fr->pinned = false;
            break;
        }
    }
    
    if (!already_holding_lock)
        lock_release (&frame_lock);
}