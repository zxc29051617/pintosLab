#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t swap_index_t;

/* 一個 swap slot 包含 8 個 sector == 4 KiB */
#define SECTOR_PER_PAGE 8

// Forward declaration
struct suppPage;

/* 初始化：在 vm_init() 或 frame_init() 之後呼叫一次 */
void vm_swap_init (void);

/* 將 page->frame 的內容寫到 swap，回傳 true => 成功。
   成功後 caller 應把 frame 釋放 (frame_free)。 */
bool swap_out (struct suppPage *page);

/* 從 swap slot 讀回到給定 kva。回傳 true 成功。 */
bool swap_in  (struct suppPage *page, void *kva);

/* 釋放尚未 swap_in 的 slot */
void vm_swap_free (swap_index_t swap_index);

#endif /* VM_SWAP_H */