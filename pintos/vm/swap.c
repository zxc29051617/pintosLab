/* vm/swap.c  ── 最小可編譯 stub，尚未處理 error 邊界與 read/write 失敗 */

#include "vm/swap.h"
#include "vm/page.h"
#include "vm/frame.h"     /* ← 新增：取得 struct frame 具體定義 */
#include "threads/synch.h"
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "threads/thread.h"
#include "threads/interrupt.h"  /* 新增：中斷相關定義 */
#include <string.h>
#include <bitmap.h>
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "lib/stdio.h"
#include "lib/debug.h"

static struct block *swap_block;        /* 指向 swap 區塊裝置 */
static struct bitmap *swap_available;   /* 管理 slot 使用情況 */
static struct lock   swap_lock;         /* 所有 swap 操作的鎖 */

static const size_t SECTORS_PER_PAGE = PGSIZE / BLOCK_SECTOR_SIZE;

// 可能的（swap）page數量
static size_t swap_size;

// 如果沒有真正的swap分區，使用內存中的緩衝區模擬
#define DEFAULT_SWAP_SIZE 1024  // 默認支持 1024 頁swap空間
static void **swap_memory_map;   // 內存映射swap區（當真實設備不可用時）
static bool using_memory_swap;   // 是否使用內存模擬swap

/*------------------------------------------------------------*/

void
vm_swap_init (void)
{
    ASSERT (SECTORS_PER_PAGE > 0); // 4096/512 = 8?

    // 初始化swap磁盤
    swap_block = block_get_role (BLOCK_SWAP);
    
    if (swap_block == NULL) {
        printf ("警告：無法獲取swap設備，將使用內存模擬swap\n");
        using_memory_swap = true;
        
        // 創建內存映射swap區
        swap_memory_map = malloc(DEFAULT_SWAP_SIZE * sizeof(void*));
        if (swap_memory_map == NULL)
            PANIC("cannot allocate memory for swap");
            
        memset(swap_memory_map, 0, DEFAULT_SWAP_SIZE * sizeof(void*));
        swap_size = DEFAULT_SWAP_SIZE;
    } else {
        using_memory_swap = false;
        // 使用真實swap分區
        swap_size = block_size (swap_block) / SECTORS_PER_PAGE;
    }
    
    // 初始化 swap_available，所有條目都為 true
    swap_available = bitmap_create (swap_size);
    if (swap_available == NULL)
        PANIC ("無法創建 swap_available 位圖");
        
    bitmap_set_all (swap_available, true);
    lock_init (&swap_lock);
    
    printf ("swap區初始化完成: %zu 頁, %s\n", 
           swap_size, 
           using_memory_swap ? "使用內存模擬" : "使用真實swap分區");
}

/* 將 page->frame 內容寫到 swap；成功回傳 true */

bool
swap_out (struct suppPage *page)
{
    if (page == NULL || page->frame == NULL)
        return false;
        
    // 先獲取一個 swap slot
    lock_acquire (&swap_lock);
    size_t slot = bitmap_scan_and_flip (swap_available, 0, 1, false);
    lock_release (&swap_lock);

    if (slot == BITMAP_ERROR)
        return false;                           /* swap full */

    // 在進行 I/O 操作之前先確保page被 pin，避免在 I/O 期間被踢掉
    void *kva = page->frame->kva;
    vm_frame_pin(kva);

    // 保存當前中斷狀態
    enum intr_level old_level = intr_get_level();
    
    if (using_memory_swap) {
       
        // 確保中斷是啟用的
        if (old_level == INTR_OFF)
            intr_set_level(INTR_ON);
            
        // 為此槽分配內存
        swap_memory_map[slot] = malloc(PGSIZE);
        if (swap_memory_map[slot] == NULL) {
            // 恢復中斷級別
            if (old_level == INTR_OFF)
                intr_set_level(INTR_OFF);
                
            // 釋放 pin 和swap槽
            vm_frame_unpin(kva);
            lock_acquire (&swap_lock);
            bitmap_reset (swap_available, slot);
            lock_release (&swap_lock);
            return false;
        }
            
        // 複製frame內容到swap內存
        memcpy(swap_memory_map[slot], kva, PGSIZE);
    } else {
        /* 實際寫 8 sector */
        // 確保中斷是啟用的
        if (old_level == INTR_OFF)
            intr_set_level(INTR_ON);
            
        for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
            block_write (swap_block,
                         slot * SECTORS_PER_PAGE + i,
                         kva + i * BLOCK_SECTOR_SIZE);
    }

    // 恢復中斷級別
    if (old_level == INTR_OFF)
        intr_set_level(INTR_OFF);

    // I/O 操作完成後 unpin page
    vm_frame_unpin(kva);

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
    if (slot >= swap_size)
        return false;                            /* 無效的 slot */

    // 在進行 I/O 操作之前先確保page被 pin
    vm_frame_pin(kva);

    // 保存當前中斷狀態
    enum intr_level old_level = intr_get_level();

    if (using_memory_swap) {
        // 使用內存模擬swap
        // 確保中斷是啟用的
        if (old_level == INTR_OFF)
            intr_set_level(INTR_ON);
            
        if (swap_memory_map[slot] == NULL) {
            // 恢復中斷級別
            if (old_level == INTR_OFF)
                intr_set_level(INTR_OFF);
                
            // 釋放 pin
            vm_frame_unpin(kva);
            return false;
        }
            
        // 從swap內存複製到frame
        memcpy(kva, swap_memory_map[slot], PGSIZE);
        
        // 釋放swap內存
        free(swap_memory_map[slot]);
        swap_memory_map[slot] = NULL;
    } else {
        // 確保中斷是啟用的
        if (old_level == INTR_OFF)
            intr_set_level(INTR_ON);
            
        for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
            block_read (swap_block,
                        slot * SECTORS_PER_PAGE + i,
                        kva + i * BLOCK_SECTOR_SIZE);
    }

    // 恢復中斷級別
    if (old_level == INTR_OFF)
        intr_set_level(INTR_OFF);

    // I/O 操作完成後 unpin page
    vm_frame_unpin(kva);

    /* 釋放 bitmap */
    lock_acquire (&swap_lock);
    bitmap_reset (swap_available, slot);
    lock_release (&swap_lock);

    page->in_swap = false;
    return true;
}

void
vm_swap_free (swap_index_t swap_index)
{
    // 檢查swap區域
    ASSERT (swap_index < swap_size);
    
    lock_acquire (&swap_lock);
    if (bitmap_test (swap_available, swap_index) == true) {
        lock_release (&swap_lock);
        PANIC ("wrong swap");
    }
    
    // 如果使用內存模擬swap，釋放相應的內存
    if (using_memory_swap && swap_memory_map[swap_index] != NULL) {
        free(swap_memory_map[swap_index]);
        swap_memory_map[swap_index] = NULL;
    }
    
    bitmap_set (swap_available, swap_index, true);
    lock_release (&swap_lock);
}