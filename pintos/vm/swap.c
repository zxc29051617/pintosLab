/* vm/swap.c  ── 最小可編譯 stub，尚未處理 error 邊界與 read/write 失敗 */

#include "vm/swap.h"
#include "vm/page.h"
#include "vm/frame.h"     /* ← 新增：取得 struct frame 具體定義 */
#include "threads/synch.h"
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "threads/thread.h"
#include <string.h>

static struct block *swap_block;        /* 指向 swap 區塊裝置 */
static struct bitmap *swap_bitmap;      /* 管理 slot 使用情況 */
static struct lock   swap_lock;         /* 所有 swap 操作的鎖 */

/*------------------------------------------------------------*/

void
swap_init (void)
{
    swap_block = block_get_role (BLOCK_SWAP);
    if (swap_block == NULL)
        PANIC ("No swap block device; check pintos -q run");

    /* 一個 slot = 8 sector */
    size_t page_cnt = block_size (swap_block) / SECTOR_PER_PAGE;
    swap_bitmap = bitmap_create (page_cnt);
    if (swap_bitmap == NULL)
        PANIC ("swap bitmap alloc failed");

    lock_init (&swap_lock);
}

/* 將 page->frame 內容寫到 swap；成功回傳 true */

bool
swap_out (struct suppPage *page)
{
    lock_acquire (&swap_lock);
    size_t slot = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
    lock_release (&swap_lock);

    if (slot == BITMAP_ERROR)
        return false;                           /* swap full */

    /* 實際寫 8 sector */
    void *kva = page->frame->kva;
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
        block_write (swap_block,
                     slot * SECTOR_PER_PAGE + i,
                     kva + i * BLOCK_SECTOR_SIZE);

    /* 標記 page 狀態 */
    page->in_swap  = true;
    page->swap_slot = slot;
    return true;
}

/* 從 swap_slot 讀回到 kva；成功回傳 true */

bool
swap_in (struct suppPage *page, void *kva)
{
    if (!page->in_swap)
        return false;                            /* 不在 swap */

    size_t slot = page->swap_slot;

    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
        block_read (swap_block,
                    slot * SECTOR_PER_PAGE + i,
                    kva + i * BLOCK_SECTOR_SIZE);

    /* 釋放 bitmap */
    lock_acquire (&swap_lock);
    bitmap_reset (swap_bitmap, slot);
    lock_release (&swap_lock);

    page->in_swap = false;
    return true;
}

/* 無條件釋放指定 slot（沒讀回就結束行程時用） */
void
swap_free (size_t slot_idx)
{
    lock_acquire (&swap_lock);
    bitmap_reset (swap_bitmap, slot_idx);
    lock_release (&swap_lock);
}