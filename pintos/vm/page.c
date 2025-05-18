#include "vm/page.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include <stdbool.h>
#include <stddef.h>
#include "threads/thread.h"

#include "threads/palloc.h"
#include "userprog/pagedir.h"   // 依照你系統的 page table header

#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"

#include <string.h>
#include <stdio.h>

/* Hash function for suppPage */
static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED) {
    struct suppPage *page = hash_entry(e, struct suppPage, hash_elem);
    return hash_bytes(&page->va, sizeof page->va);
}

/* Comparison function for suppPage */
static bool page_less(const struct hash_elem *a,
                     const struct hash_elem *b, void *aux UNUSED) {
    struct suppPage *page_a = hash_entry(a, struct suppPage, hash_elem);
    struct suppPage *page_b = hash_entry(b, struct suppPage, hash_elem);
    return page_a->va < page_b->va;
}

/* Initialize supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt) {
    hash_init(&spt->spt_hash, page_hash, page_less, NULL);
}

/* Destroy supplemental page table and free all suppPages */
static void page_destroy(struct hash_elem *e, void *aux UNUSED) {
    struct suppPage *page = hash_entry(e, struct suppPage, hash_elem);
    if (page->frame) 
        palloc_free_page (page->frame); /* 把 frame 釋放回 core pool */
    free(page);
}

void supplemental_page_table_destroy(struct supplemental_page_table *spt) {
    hash_destroy(&spt->spt_hash, page_destroy);
}

/* Insert a suppPage to SPT; return true on success, false if va exists */
bool spt_insert_page(struct supplemental_page_table *spt, struct suppPage *page) {
    struct hash_elem *result = hash_insert(&spt->spt_hash, &page->hash_elem);
    return result == NULL; // NULL means insert succeeded
}

bool
vm_alloc_page(enum vm_type type, void *upage, bool writable) {
    struct supplemental_page_table *spt = &thread_current()->spt;
    upage = pg_round_down(upage);

    // 1. 查重
    if (spt_find_page(spt, upage) != NULL)
        return false;

    // 2. 建一個 suppPage
    struct suppPage *page = malloc(sizeof(struct suppPage));
    if (page == NULL)
        return false;

    page->va = upage;
    page->writable = writable;
    page->frame = NULL;
    page->type = type;    // struct suppPage 有 type
    page->pinned = false; // ← 順便初始化
    page->in_swap   = false;          /* 一開始不在 swap */
    page->swap_slot = (size_t) -1;    /* -1 表示「尚未分配 slot」*/

    // 3. 插入
    if (!spt_insert_page(spt, page)) {
        free(page);
        return false;
    }
    return true;
}

/* Find suppPage in SPT by virtual address, return NULL if not found */
struct suppPage *spt_find_page(struct supplemental_page_table *spt, void *va) {
    struct suppPage temp;
    temp.va = pg_round_down(va); // 保證頁對齊
    struct hash_elem *e = hash_find(&spt->spt_hash, &temp.hash_elem);
    if (e == NULL) return NULL;
    return hash_entry(e, struct suppPage, hash_elem);
}

/* Remove a suppPage from SPT and free its memory */
void spt_remove_page(struct supplemental_page_table *spt, struct suppPage *page) {
    hash_delete(&spt->spt_hash, &page->hash_elem);
    free(page);
}

/* Claim a page: load it into a physical frame and install into pagedir.
   Return true on success, false on failure. */
 
bool
vm_do_claim_page (struct suppPage *page)
{
    printf("[DEBUG] Claim page: va=%p, type=%d, init=%p\n", page->va, page->type, page->initializer);
    struct frame *fr = frame_allocate (PAL_USER | PAL_ZERO);
    if (fr == NULL)
        return false;

    fr->page   = page;
    fr->owner  = thread_current();
    fr->pinned = true;
    page->frame = fr;
    page->pinned = true;
    void *kva = fr->kva;

    // ------- NEW: Lazy loading 支援 -----------
    if (page->initializer) {
        // 呼叫 initializer（如 lazy_load_segment）
        if (!page->initializer(page, page->aux))
            goto fail;
        // lazy_load 完成後就可以把這兩個 pointer 清掉
        page->initializer = NULL;
        page->aux = NULL;
    } else {
        // 舊 eager code
        switch (page->type) {
            case VM_ANON:
            case VM_STACK:
                if (page->in_swap) {
                    if (!swap_in (page, kva))
                        goto fail;
                    page->in_swap = false;
                }
                break;
            case VM_FILE:
                if (file_read_at (page->file, kva,
                                  page->read_bytes, page->ofs) != (int) page->read_bytes)
                    goto fail;
                memset (kva + page->read_bytes, 0, page->zero_bytes);
                break;
            default:
                goto fail;
        }
    }

    struct thread *cur = thread_current ();
    if (!pagedir_set_page (cur->pagedir, page->va, kva, page->writable))
        goto fail;
    if (page->type == VM_FILE)
        pagedir_set_dirty (cur->pagedir, page->va, false);

    fr->pinned   = false;
    page->pinned = false;
    return true;

fail:
    fr->pinned = false;
    frame_free (fr);
    page->frame = NULL;
    return false;
}

void vm_pin_page(void *va) {
    struct supplemental_page_table *spt = &thread_current()->spt;
    struct suppPage *page = spt_find_page(spt, va);
    if (page && page->frame)
        page->pinned = true;
}

void vm_unpin_page(void *va) {
    struct supplemental_page_table *spt = &thread_current()->spt;
    struct suppPage *page = spt_find_page(spt, va);
    if (page && page->frame)
        page->pinned = false;
}

void vm_pin_buffer(const void *buf, size_t size) {
    // 對 [buf, buf+size-1] 所跨的所有頁都 pin
    const uint8_t *start = pg_round_down(buf);
    const uint8_t *end = pg_round_down((const uint8_t *)buf + size - 1);
    for (const uint8_t *p = start; p <= end; p += PGSIZE) {
        vm_pin_page((void *)p);
    }
}

void vm_unpin_buffer(const void *buf, size_t size) {
    // 對 [buf, buf+size-1] 所跨的所有頁都 unpin
    const uint8_t *start = pg_round_down(buf);
    const uint8_t *end = pg_round_down((const uint8_t *)buf + size - 1);
    for (const uint8_t *p = start; p <= end; p += PGSIZE) {
        vm_unpin_page((void *)p);
    }
}

bool
lazy_load_segment(struct suppPage *page, void *aux)
{
    struct file_page *fpage = aux;
    off_t offset = fpage->ofs;
    uint32_t read_bytes = fpage->read_bytes;
    uint32_t zero_bytes = fpage->zero_bytes;

    // 安全檢查
    if (page->frame == NULL || page->frame->kva == NULL) return false;

    void *kva = page->frame->kva;
    // 1. 從 file 讀檔進 frame
    if (file_read_at(fpage->file, kva, read_bytes, offset) != (int)read_bytes) {
        free(fpage); // <--- 失敗也要 free
        return false;
    }
    // 2. 補 0
    memset(kva + read_bytes, 0, zero_bytes);
    free(fpage); // <--- 成功也要 free
    return true;
}


bool
vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                              vm_initializer init, void *aux)
{
    struct supplemental_page_table *spt = &thread_current()->spt;
    upage = pg_round_down(upage);

    // 查重
    if (spt_find_page(spt, upage) != NULL)
        return false;

    struct suppPage *page = malloc(sizeof(struct suppPage));
    if (page == NULL)
        return false;

    page->va = upage;
    page->writable = writable;
    page->frame = NULL;
    page->type = type;
    page->pinned = false;
    page->in_swap   = false;
    page->swap_slot = (size_t)-1;

    // lazy loading 重要欄位
    page->initializer = init;
    page->aux = aux;

    // VM_FILE info 可由 lazy_load 裡 aux 補充，也可以預設 NULL
    page->file = NULL;
    page->ofs = 0;
    page->read_bytes = 0;
    page->zero_bytes = 0;

    if (!spt_insert_page(spt, page)) {
        free(page);
        return false;
    }
    return true;
}
