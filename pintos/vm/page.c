#include "vm/page.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include <stdbool.h>
#include <stddef.h>
#include "threads/thread.h"
#include "threads/interrupt.h"  /* 添加中斷相關頭文件 */

#include "threads/palloc.h"
#include "userprog/pagedir.h"   // 依照你系統的 page table header

#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"

#include <string.h>
#include <stdio.h>

#include <hash.h>
#include "lib/kernel/hash.h"

#include "threads/synch.h"

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
    hash_init(&spt->page_map, page_hash, page_less, NULL);
}

/* Destroy supplemental page table and free all suppPages */
static void page_destroy(struct hash_elem *e, void *aux UNUSED) {
    struct suppPage *page = hash_entry(e, struct suppPage, hash_elem);
    if (page->frame) {
        vm_frame_free(page->frame);
    }
    free(page);
}

void supplemental_page_table_destroy(struct supplemental_page_table *spt) {
    hash_destroy(&spt->page_map, page_destroy);
}

/* Insert a suppPage to SPT; return true on success, false if va exists */
bool spt_insert_page(struct supplemental_page_table *spt, struct suppPage *page) {
    struct hash_elem *result = hash_insert(&spt->page_map, &page->hash_elem);
    return result == NULL; // NULL means insert succeeded
}

bool
vm_alloc_page(enum vm_type type, void *upage, bool writable) {
    struct thread *t = thread_current();
    struct supplemental_page_table *spt = t->spt;
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
    // 在插入 SPT 之前暫時啟用中斷，因為可能涉及到鎖操作
    enum intr_level old_level = intr_get_level();
    if (old_level == INTR_OFF)
        intr_set_level(INTR_ON);
        
    bool success = spt_insert_page(spt, page);
    
    // 恢復原來的中斷級別
    if (old_level == INTR_OFF)
        intr_set_level(INTR_OFF);
        
    if (!success) {
        free(page);
        return false;
    }
    return true;
}

/* Find suppPage in SPT by virtual address, return NULL if not found */
struct suppPage *spt_find_page(struct supplemental_page_table *spt, void *va) {
    struct suppPage temp;
    temp.va = pg_round_down(va); // 保證頁對齊
    struct hash_elem *e = hash_find(&spt->page_map, &temp.hash_elem);
    if (e == NULL) return NULL;
    return hash_entry(e, struct suppPage, hash_elem);
}

/* Remove a suppPage from SPT and free its memory */
void spt_remove_page(struct supplemental_page_table *spt, struct suppPage *page) {
    hash_delete(&spt->page_map, &page->hash_elem);
    free(page);
}

/* Claim a page: load it into a physical frame and install into pagedir.
   Return true on success, false on failure. */
 
bool
vm_do_claim_page (struct suppPage *page)
{
    if (page == NULL)
        return false;
        
    // 首先檢查page是否已被pin住，避免重複操作
    if (page->pinned)
        return false;
        
    // 分配物理frame，這不需要中斷啟用
    struct frame *frame = vm_frame_allocate (PAL_USER | PAL_ZERO, page->va);
    if (frame == NULL)
        return false;
        
    void *kva = frame->kva;

    // 設置page和frame的關聯，但不立即設置 pinned 標誌
    page->frame = frame;
    frame->page = page;
    frame->owner = thread_current();
    
    // 使用 vm_frame_pin 來確保frame被正確pin住
    vm_frame_pin(kva);
    page->pinned = true;

    bool success = false;
    
    // 保存當前中斷狀態
    enum intr_level old_level = intr_get_level();
    
    // ------- 處理page內容加載 -----------
    if (page->initializer) {
        // 臨時啟用中斷
        if (old_level == INTR_OFF)
            intr_set_level(INTR_ON);
        
        // 呼叫 initializer（如 lazy_load_segment）
        success = page->initializer(page, page->aux);
        
        // 恢復原來的中斷狀態
        if (old_level == INTR_OFF)
            intr_set_level(INTR_OFF);
        
        if (!success) {
            // 解除pin住page，釋放frame
            vm_frame_unpin(kva);
            page->pinned = false;
            vm_frame_free(kva);
            page->frame = NULL;
            return false;
        }
        
        // lazy_load 完成後就可以把這兩個 pointer 清掉
        page->initializer = NULL;
        page->aux = NULL;
    } else {
        // 臨時啟用中斷
        if (old_level == INTR_OFF)
            intr_set_level(INTR_ON);
        
        // eager code
        switch (page->type) {
            case VM_ANON:
            case VM_STACK:
                if (page->in_swap) {
                    success = swap_in(page, kva);
                    if (success) {
                        page->in_swap = false;
                    }
                } else {
                    success = true;  // 新分配的page已經被 PAL_ZERO 清零
                }
                break;
            case VM_FILE:
                if (file_read_at(page->file, kva, page->read_bytes, page->ofs) != (int) page->read_bytes) {
                    success = false;
                } else {
                    memset(kva + page->read_bytes, 0, page->zero_bytes);
                    success = true;
                }
                break;
            default:
                success = false;
        }
        
        // 恢復原來的中斷狀態
        if (old_level == INTR_OFF)
            intr_set_level(INTR_OFF);
        
        if (!success) {
            // 解除pin住page，釋放frame
            vm_frame_unpin(kva);
            page->pinned = false;
            vm_frame_free(kva);
            page->frame = NULL;
            return false;
        }
    }

    struct thread *cur = thread_current();
    // 設置頁表映射前再次確認
    if (!pagedir_set_page(cur->pagedir, page->va, kva, page->writable)) {
        // 解除pin住page，釋放frame
        vm_frame_unpin(kva);
        page->pinned = false;
        vm_frame_free(kva);
        page->frame = NULL;
        return false;
    }
    
    if (page->type == VM_FILE)
        pagedir_set_dirty(cur->pagedir, page->va, false);

    // 解除pin住page
    vm_frame_unpin(kva);
    page->pinned = false;
    return true;
}

void vm_pin_buffer(const void *buf, size_t size) {
    if (buf == NULL || size == 0) return;
    
    // 對 [buf, buf+size-1] 所跨的所有頁都 pin
    const uint8_t *start = pg_round_down(buf);
    const uint8_t *end = pg_round_down((const uint8_t *)buf + size - 1);
    struct supplemental_page_table *supt = thread_current()->spt;
    
    if (supt == NULL) return;
    
    // 確保所有page都在內存中並且被pin住
    for (const uint8_t *p = start; p <= end; p += PGSIZE) {
        struct suppPage *page = spt_find_page(supt, (void *)p);
        if (page == NULL) {
            // 如果page不存在，可能是堆疊增長或無效訪問
            if (p >= (const uint8_t *)((char *)thread_current()->current_esp - 32) &&
                p < (const uint8_t *)PHYS_BASE) {
                // 似乎是堆疊訪問，嘗試創建新page
                vm_alloc_page(VM_STACK, (void *)p, true);
                page = spt_find_page(supt, (void *)p);
            }
        }
        
        if (page != NULL) {
            // pin住page
            vm_pin_page(supt, (void *)p);
            
            // 如果page不在內存中，載入它
            if (page->frame == NULL && !page->in_swap) {
                vm_do_claim_page(page);
            }
        }
    }
}

void vm_unpin_buffer(const void *buf, size_t size) {
    if (buf == NULL || size == 0) return;
    
    // 對 [buf, buf+size-1] 所跨的所有頁都 unpin
    const uint8_t *start = pg_round_down(buf);
    const uint8_t *end = pg_round_down((const uint8_t *)buf + size - 1);
    struct supplemental_page_table *supt = thread_current()->spt;
    
    if (supt == NULL) return;
    
    for (const uint8_t *p = start; p <= end; p += PGSIZE) {
        vm_unpin_page(supt, (void *)p);
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
    struct thread *t = thread_current();
    struct supplemental_page_table *spt = t->spt;
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

static unsigned spte_hash_func(const struct hash_elem *elem, void *aux UNUSED);
static bool     spte_less_func(const struct hash_elem *, const struct hash_elem *, void *aux);
static void     spte_destroy_func(struct hash_elem *elem, void *aux);

struct supplemental_page_table*
vm_supt_create (void)
{
  struct supplemental_page_table *spt = 
    (struct supplemental_page_table*) malloc(sizeof(struct supplemental_page_table));
  supplemental_page_table_init(spt);
  return spt;
}

void
vm_supt_destroy (struct supplemental_page_table *spt)
{
  supplemental_page_table_destroy(spt);
  free(spt);
}

struct supplemental_page_table_entry* 
vm_supt_lookup (struct supplemental_page_table *spt, void *va)
{
  struct suppPage *page = spt_find_page(spt, va);
  if (!page) return NULL;
  
  static struct supplemental_page_table_entry spte;
  spte.upage = page->va;
  spte.kpage = page->frame ? page->frame->kva : NULL;
  spte.status = page->in_swap ? ON_SWAP : 
                (page->frame ? ON_FRAME : 
                 (page->file ? FROM_FILESYS : ALL_ZERO));
  spte.dirty = false; // 默認情況 通常會被 set_dirty 更新
  spte.swap_index = page->swap_slot;
  spte.file = page->file;
  spte.file_offset = page->ofs;
  spte.read_bytes = page->read_bytes;
  spte.zero_bytes = page->zero_bytes;
  spte.writable = page->writable;
  
  return &spte;
}

bool 
vm_supt_has_entry (struct supplemental_page_table *spt, void *va)
{
  return spt_find_page(spt, va) != NULL;
}

bool 
vm_supt_set_dirty (struct supplemental_page_table *spt, void *va, bool value)
{
  struct suppPage *page = spt_find_page(spt, va);
  if (!page) return false;
  
  if (page->frame && thread_current()->pagedir)
    pagedir_set_dirty(thread_current()->pagedir, va, value);
  
  return true;
}

bool 
vm_load_page(struct supplemental_page_table *spt, uint32_t *pagedir, void *upage)
{
  struct suppPage *page = spt_find_page(spt, upage);
  if (!page) return false;
  
  return vm_do_claim_page(page);
}

void
vm_pin_page(struct supplemental_page_table *supt, void *page)
{
    if (supt == NULL || page == NULL) return;
    
    struct suppPage *p = spt_find_page(supt, page);
    if (p == NULL) return;
    
    // 標記page為已pin住
    p->pinned = true;
    
    // 如果page已經在內存中，則pin住其frame
    if (p->frame != NULL && p->frame->kva != NULL) {
        vm_frame_pin(p->frame->kva);
    } else if (!p->in_swap) {
        // 如果page不在內存中且不在swap區，則嘗試加載它
        vm_do_claim_page(p);
    }
}

void
vm_unpin_page(struct supplemental_page_table *supt, void *page)
{
    if (supt == NULL || page == NULL) return;
    
    struct suppPage *p = spt_find_page(supt, page);
    if (p == NULL) return;
    
    // 標記page為未pin住
    p->pinned = false;
    
    // 如果page已經在內存中，則解除其frame的pin住
    if (p->frame != NULL && p->frame->kva != NULL) {
        vm_frame_unpin(p->frame->kva);
    }
}

/**
 * 安裝一個當前在frame上的page（由起始地址 `upage` 指定）到補充頁表中。
 */
bool
vm_supt_install_frame (struct supplemental_page_table *spt, void *upage, void *kpage)
{
  return vm_alloc_page(VM_ANON, upage, true);
}

/**
 * 在補充頁表中安裝一個新page（由起始地址 `upage` 指定）。
 * 該page類型為 ALL_ZERO，表示所有字節都是（懶惰地）為零。
 */
bool
vm_supt_install_zeropage (struct supplemental_page_table *spt, void *upage)
{
  return vm_alloc_page(VM_ANON, upage, true);
}

/**
 * 將現有page標記為已swap出，並在 SPTE 中更新 swap_index。
 */
bool
vm_supt_set_swap (struct supplemental_page_table *spt, void *page, swap_index_t swap_index)
{
  struct suppPage *p = spt_find_page(spt, page);
  if (!p) return false;
  
  p->in_swap = true;
  p->swap_slot = swap_index;
  return true;
}

/**
 * 在補充頁表中安裝一個新page（由起始地址 `upage` 指定），類型為 FROM_FILESYS。
 */
bool
vm_supt_install_filesys (struct supplemental_page_table *spt, void *upage,
    struct file *file, off_t offset, uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct file_page *aux = malloc(sizeof(struct file_page));
  if (!aux) return false;
  
  aux->file = file;
  aux->ofs = offset;
  aux->read_bytes = read_bytes;
  aux->zero_bytes = zero_bytes;
  
  return vm_alloc_page_with_initializer(VM_FILE, upage, writable, lazy_load_segment, aux);
}

/**
 * 取消映射page。如果page已dirty，則將page內容寫回文件。
 */
bool
vm_supt_mm_unmap(
    struct supplemental_page_table *spt, uint32_t *pagedir,
    void *page, struct file *f, off_t offset, size_t bytes)
{
  struct suppPage *p = spt_find_page(spt, page);
  if (!p) return false;
  
  // 如果page在記憶體中，檢查是否需要寫回
  if (p->frame) {
    bool is_dirty = pagedir_is_dirty(pagedir, p->va);
    
    // 如果是檔案映射且為dirty，寫回
    if (is_dirty && p->type == VM_FILE && f != NULL) {
      file_write_at(f, p->frame->kva, bytes, offset);
    }
    
    // 取消page映射
    pagedir_clear_page(pagedir, p->va);
    vm_frame_free(p->frame);
  } 
  else if (p->in_swap) {
    // 如果在swap區中，釋放swap槽
    vm_swap_free(p->swap_slot);
  }
  
  // 從 SPT 中移除
  spt_remove_page(spt, p);
  
  return true;
}
