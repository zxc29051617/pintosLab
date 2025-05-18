#include "vm/frame.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"
#include <debug.h> 

/* ---------- 全域物件 ---------- */
static struct list frame_table;       /* 雙向循環 list — 保存所有 frame */
static struct lock frame_lock;        /* 保護 frame_table + clock_hand */
static struct list_elem *clock_hand;  /* 指向下一個要檢查的 frame     */

/* ---------- 工具函式 ---------- */

/* 將 list_elem 轉回 struct frame* */
static inline struct frame *
elem_to_frame (struct list_elem *e)
{
    return list_entry (e, struct frame, elem);
}

/* Clock algorithm：找到一塊「un-pinned & accessed=0」的 frame */
static struct frame *
select_victim (void)
{
    size_t table_sz = list_size (&frame_table);
    if (table_sz == 0) return NULL;

    /* 從 clock_hand 開始繞，只要繞過的 elem 數 >= 表大小就保證看過全部 */
    for (size_t scanned = 0; scanned < table_sz * 2; scanned++)
    {
        if (clock_hand == list_end (&frame_table))
            clock_hand = list_begin (&frame_table);

        struct frame *fr = elem_to_frame (clock_hand);
        clock_hand = list_next (clock_hand);        /* hand 往前移 */

        if (fr->pinned)        /* 被釘住 => 跳過 */
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
    ASSERT (victim && victim->page);

    struct suppPage *page = victim->page;
    struct thread   *owner = victim->owner;

    /* 先在 pagedir 斷開映射，避免 race */
    pagedir_clear_page (owner->pagedir, page->va);

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
    if (ok)
        page->frame = NULL;            /* 斷聯繫，頁狀態已更新 */

    return ok;
}

/* ---------- 對外 API ---------- */

void
frame_table_init (void)
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = list_end (&frame_table);
}

/* 分配一塊 user frame；若分配失敗會嘗試驅逐一塊 frame */
struct frame *
frame_allocate (enum palloc_flags flags)
{
    ASSERT (flags & PAL_USER);

    lock_acquire (&frame_lock);

    /* 1. 先直接向 palloc 要 */
    void *kva = palloc_get_page (flags);
    if (kva == NULL)
    {
        /* 2. OOM → 找 victim 驅逐 */
        struct frame *victim = select_victim ();
        if (victim == NULL || !evict_frame (victim))
            PANIC ("frame eviction failed (all frames pinned or swap full)");

        kva = victim->kva;            /* 直接重用 victim 的 page */
        list_remove (&victim->elem);
        free (victim);
    }

    /* 3. 建立新 frame 結構 */
    struct frame *fr = malloc (sizeof *fr);
    if (fr == NULL)
        PANIC ("frame struct alloc failed");

    fr->kva   = kva;
    fr->page  = NULL;         /* claim_page() 再填 */
    fr->owner = thread_current ();
    fr->pinned = false;

    list_push_back (&frame_table, &fr->elem);

    lock_release (&frame_lock);
    return fr;
}

void
frame_free (struct frame *fr)
{
    if (fr == NULL) return;

    lock_acquire (&frame_lock);
    list_remove (&fr->elem);
    palloc_free_page (fr->kva);
    free (fr);
    lock_release (&frame_lock);
}

/* -------- 快速 pin / unpin（外部呼叫） -------- */
void
frame_pin (struct frame *fr)
{
    lock_acquire (&frame_lock);
    fr->pinned = true;
    lock_release (&frame_lock);
}

void
frame_unpin (struct frame *fr)
{
    lock_acquire (&frame_lock);
    fr->pinned = false;
    lock_release (&frame_lock);
}