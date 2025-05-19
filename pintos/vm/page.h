#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/kernel/hash.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "filesys/off_t.h"
#include "vm/swap.h"

#ifndef VM_VM_H
#define VM_VM_H


void vm_init (void);

#endif 

/* ---------- 前置宣告 ---------- */
struct suppPage;
struct frame;
struct file;

// --- Lazy loading 用 typedef/struct ---
typedef bool (*vm_initializer)(struct suppPage *, void *aux);

struct file_page {
    struct file *file;
    off_t ofs;
    size_t read_bytes;
    size_t zero_bytes;
};


enum vm_type {
    VM_ANON = 0,
    VM_FILE,
    VM_STACK
};

/**
 * page狀態表示
 */
enum page_status {
  ALL_ZERO,         // 全零頁
  ON_FRAME,         // 在記憶體中
  ON_SWAP,          // 在swap空間
  FROM_FILESYS      // 從檔案系統加載
};


struct supplemental_page_table {
    struct hash page_map;
};

struct supplemental_page_table_entry {
    void *upage;              /* 虛擬地址（鍵值） */
    void *kpage;              /* 關聯的內核page
                                 只有當 status == ON_FRAME 時有效
                                 如果page不在記憶體frame中，應為 NULL */
    struct hash_elem elem;

    enum page_status status;

    bool dirty;               /* dirty位 */

    // 用於 ON_SWAP
    swap_index_t swap_index;  /* 如果page被swap出去，存儲swap索引
                                 只有當 status == ON_SWAP 時有效 */

    // 用於 FROM_FILESYS
    struct file *file;
    off_t file_offset;
    uint32_t read_bytes, zero_bytes;
    bool writable;
};

struct suppPage {
    void *va;                       
    struct hash_elem hash_elem;
    enum vm_type type;
    bool  writable;
    struct frame *frame;            
    bool   in_swap;
    size_t swap_slot;

    // File-backed info (VM_FILE)
    struct file *file;
    off_t  ofs;
    size_t read_bytes;
    size_t zero_bytes;

    bool pinned;

    // --- Lazy Loading 欄位 ---
    vm_initializer initializer;  // 如果非 NULL 就 lazy load
    void *aux;                   // 傳給 initializer 的參數
};

void   supplemental_page_table_init (struct supplemental_page_table *);
void   supplemental_page_table_destroy (struct supplemental_page_table *);
bool   spt_insert_page (struct supplemental_page_table *, struct suppPage *);
struct suppPage *spt_find_page (struct supplemental_page_table *, void *va);
void   spt_remove_page (struct supplemental_page_table *, struct suppPage *);

bool   vm_alloc_page (enum vm_type type, void *upage, bool writable);
bool   vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                   vm_initializer init, void *aux);
bool   vm_do_claim_page (struct suppPage *page);

bool lazy_load_segment(struct suppPage *page, void *aux);

void vm_pin_buffer(const void *buf, size_t size);
void vm_unpin_buffer(const void *buf, size_t size);

/*
 * 操作補充頁表的方法
 */

struct supplemental_page_table* vm_supt_create (void);
void vm_supt_destroy (struct supplemental_page_table *);

bool vm_supt_install_frame (struct supplemental_page_table *supt, void *upage, void *kpage);
bool vm_supt_install_zeropage (struct supplemental_page_table *supt, void *);
bool vm_supt_set_swap (struct supplemental_page_table *supt, void *, swap_index_t);
bool vm_supt_install_filesys (struct supplemental_page_table *supt, void *page,
    struct file * file, off_t offset, uint32_t read_bytes, uint32_t zero_bytes, bool writable);

struct supplemental_page_table_entry* vm_supt_lookup (struct supplemental_page_table *supt, void *);
bool vm_supt_has_entry (struct supplemental_page_table *, void *page);

bool vm_supt_set_dirty (struct supplemental_page_table *supt, void *, bool);

bool vm_load_page(struct supplemental_page_table *supt, uint32_t *pagedir, void *upage);

bool vm_supt_mm_unmap(struct supplemental_page_table *supt, uint32_t *pagedir,
    void *page, struct file *f, off_t offset, size_t bytes);

void vm_pin_page(struct supplemental_page_table *supt, void *page);
void vm_unpin_page(struct supplemental_page_table *supt, void *page);

#endif /* VM_PAGE_H */