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
    /* ---------- 1. 取得一塊 frame ---------- */
    struct frame *fr = frame_allocate (PAL_USER | PAL_ZERO);
    if (fr == NULL)
        return false;

    /*  建立 page <-> frame 雙向連結  */
    fr->page   = page;
    fr->owner  = thread_current ();   /* 若 frame_allocate 已設就可省略 */
    fr->pinned = true;                /* 先釘住，I/O 完成後才解 */
    page->frame   = fr;
    page->pinned  = true;

    void *kva = fr->kva;

    /* ---------- 2. 依 page 類型載入 ---------- */
    switch (page->type) {
      case VM_ANON:
      case VM_STACK:
        if (page->in_swap) {
            if (!swap_in (page, kva))
                goto fail;
            page->in_swap = false;          /* ← 確定載回後清旗標 */
        }
        /* PAL_ZERO 已清好 */
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

    /* ---------- 3. 安裝 PTE ---------- */
    struct thread *cur = thread_current ();
    if (!pagedir_set_page (cur->pagedir, page->va, kva, page->writable))
        goto fail;

    /* 讀檔完成後標為乾淨 */
    if (page->type == VM_FILE)
        pagedir_set_dirty (cur->pagedir, page->va, false);

    /* ---------- 4. 解釘 ---------- */
    fr->pinned   = false;
    page->pinned = false;

    return true;

fail:
    /* 若任何步驟失敗，回收 frame，解除雙向指標 */
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
