#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/kernel/hash.h"
#include <stdbool.h>
#include <stddef.h>

#include "filesys/off_t.h" 

/* ---------- 頁面型別 ---------- */
enum vm_type {
    VM_ANON = 0,
    VM_FILE,
    VM_STACK
};

/* ---------- 前置宣告，避免互相 include ---------- */
struct frame;

/* ---------- Supplemental Page Table ---------- */
struct supplemental_page_table {
    struct hash spt_hash;
};

/* ---------- 單一頁的描述子 ---------- */
struct suppPage {
    /* ---- ❶ KEY (必須有) ---- */
    void *va;                       /* page-aligned user virtual address */
    struct hash_elem hash_elem;     /* entry for supplemental page table */

    /* ---- ❷ 基本屬性 ---- */
    enum vm_type type;
    bool  writable;

    /* ---- ❸ 與 frame 的連結 ---- */
    struct frame *frame;            /* NULL ⇒ page currently swapped-out */

    /* ---- ❹ Swap info ---- */
    bool   in_swap;
    size_t swap_slot;

    /* ---- ❺ File-backed info (VM_FILE) ---- */
    struct file *file;
    off_t  ofs;
    size_t read_bytes;
    size_t zero_bytes;

    /* ---- ❻ 其他 ---- */
    bool pinned;
};

/* ---------- Public API ---------- */
void   supplemental_page_table_init (struct supplemental_page_table *);
void   supplemental_page_table_destroy (struct supplemental_page_table *);
bool   spt_insert_page (struct supplemental_page_table *, struct suppPage *);
struct suppPage *spt_find_page (struct supplemental_page_table *, void *va);
void   spt_remove_page (struct supplemental_page_table *, struct suppPage *);

bool   vm_alloc_page (enum vm_type type, void *upage, bool writable);
bool   vm_do_claim_page (struct suppPage *page);

void   vm_pin_page   (void *va);
void   vm_unpin_page (void *va);

#endif /* VM_PAGE_H */