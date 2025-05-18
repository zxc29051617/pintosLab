#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/kernel/hash.h"
#include <stdbool.h>
#include <stddef.h>
#include "filesys/off_t.h"

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

/* ---------- 頁面型別 ---------- */
enum vm_type {
    VM_ANON = 0,
    VM_FILE,
    VM_STACK
};

/* ---------- Supplemental Page Table ---------- */
struct supplemental_page_table {
    struct hash spt_hash;
};

/* ---------- 單一頁的描述子 ---------- */
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

/* ---------- Public API ---------- */
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

void   vm_pin_page   (void *va);
void   vm_unpin_page (void *va);

void vm_pin_buffer(const void *buf, size_t size);
void vm_unpin_buffer(const void *buf, size_t size);



#endif /* VM_PAGE_H */