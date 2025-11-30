/* threads/palloc.c - 페이지 할당기 구현 (Next Fit, Best Fit, Buddy System 통합) */

#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "lib/kernel/list.h"

enum palloc_mode {
    PAL_FIRST_FIT,
    PAL_NEXT_FIT,
    PAL_BEST_FIT,
    PAL_BUDDY
};

#define MAX_ORDER 10

struct buddy_block {
    struct list_elem elem;
    size_t page_idx;
    int order;
};

struct buddy_info {
    struct list free_lists[MAX_ORDER + 1];
    size_t page_cnt;
};

struct pool
{
    struct lock lock;
    struct bitmap *used_map;
    uint8_t *base;
    
    size_t next_fit_last_idx;
    struct buddy_info *buddy_data;
};

static struct pool kernel_pool, user_pool;

static enum palloc_mode current_mode = PAL_FIRST_FIT;

static void init_pool (struct pool *, void *base, size_t page_cnt,
                       const char *name);
static bool page_from_pool (const struct pool *, void *page);

static size_t first_fit_alloc (struct pool *pool, size_t page_cnt);
static size_t next_fit_alloc (struct pool *pool, size_t page_cnt);
static size_t best_fit_alloc (struct pool *pool, size_t page_cnt);
static size_t buddy_system_alloc (struct pool *pool, size_t page_cnt);
static void buddy_system_free (struct pool *pool, size_t page_idx, size_t page_cnt);

void 
palloc_set_mode(enum palloc_mode mode)
{
    enum intr_level old_level;

    old_level = intr_disable();

    current_mode = mode;
    
    kernel_pool.next_fit_last_idx = 0;
    user_pool.next_fit_last_idx = 0;
    
    intr_set_level(old_level);
}

void
palloc_init (size_t user_page_limit)
{
    uint8_t *free_start = ptov (1024 * 1024);
    uint8_t *free_end = ptov (init_ram_pages * PGSIZE);
    size_t free_pages = (free_end - free_start) / PGSIZE;
    size_t user_pages = free_pages / 2;
    size_t kernel_pages;
    if (user_pages > user_page_limit)
        user_pages = user_page_limit;
    kernel_pages = free_pages - user_pages;

    init_pool (&kernel_pool, free_start, kernel_pages, "kernel pool");
    init_pool (&user_pool, free_start + kernel_pages * PGSIZE,
                user_pages, "user pool");
}

void *
palloc_get_multiple (enum palloc_flags flags, size_t page_cnt)
{
    struct pool *pool = flags & PAL_USER ? &user_pool : &kernel_pool;
    void *pages;
    size_t page_idx = BITMAP_ERROR; 

    if (page_cnt == 0)
        return NULL;

    lock_acquire (&pool->lock);
    
    if (page_cnt == 1 && current_mode != PAL_BUDDY) {
        page_idx = first_fit_alloc(pool, 1);
    } else {
        switch (current_mode)
        {
            case PAL_FIRST_FIT:
                page_idx = first_fit_alloc(pool, page_cnt);
                break;
            case PAL_NEXT_FIT:
                page_idx = next_fit_alloc(pool, page_cnt);
                break;
            case PAL_BEST_FIT:
                page_idx = best_fit_alloc(pool, page_cnt);
                break;
            case PAL_BUDDY:
                page_idx = buddy_system_alloc(pool, page_cnt);
                break;
            default:
                NOT_REACHED();
        }
    }

    lock_release (&pool->lock);

    if (page_idx != BITMAP_ERROR)
        pages = pool->base + PGSIZE * page_idx;
    else
        pages = NULL;

    if (pages != NULL)
        {
            if (flags & PAL_ZERO)
                memset (pages, 0, PGSIZE * page_cnt);
        }
    else
        {
            if (flags & PAL_ASSERT)
                PANIC ("palloc_get: out of pages");
        }

    return pages;
}

void *
palloc_get_page (enum palloc_flags flags)
{
    return palloc_get_multiple (flags, 1);
}

void
palloc_free_multiple (void *pages, size_t page_cnt)
{
    struct pool *pool;
    size_t page_idx;

    ASSERT (pg_ofs (pages) == 0);
    if (pages == NULL || page_cnt == 0)
        return;

    if (page_from_pool (&kernel_pool, pages))
        pool = &kernel_pool;
    else if (page_from_pool (&user_pool, pages))
        pool = &user_pool;
    else
        NOT_REACHED ();

    page_idx = pg_no (pages) - pg_no (pool->base);

#ifndef NDEBUG
    memset (pages, 0xcc, PGSIZE * page_cnt);
#endif

    ASSERT (bitmap_all (pool->used_map, page_idx, page_cnt));
    
    if (current_mode != PAL_BUDDY) {
        bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
    } else {
        buddy_system_free(pool, page_idx, page_cnt);
    }
}

void
palloc_free_page (void *page)
{
    palloc_free_multiple (page, 1);
}

static void
init_pool (struct pool *p, void *base, size_t page_cnt, const char *name)
{
    size_t bm_pages = DIV_ROUND_UP (bitmap_buf_size (page_cnt), PGSIZE);
    if (bm_pages > page_cnt)
        PANIC ("Not enough memory in %s for bitmap.", name);
    page_cnt -= bm_pages;

    printf ("%zu pages available in %s.\n", page_cnt, name);

    lock_init (&p->lock);
    p->used_map = bitmap_create_in_buf (page_cnt, base, bm_pages * PGSIZE);
    p->base = base + bm_pages * PGSIZE;
    
    p->next_fit_last_idx = 0;
    
}

static bool
page_from_pool (const struct pool *pool, void *page)
{
    size_t page_no = pg_no (page);
    size_t start_page = pg_no (pool->base);
    size_t end_page = start_page + bitmap_size (pool->used_map);

    return page_no >= start_page && page_no < end_page;
}

static size_t
first_fit_alloc (struct pool *pool, size_t page_cnt)
{
    return bitmap_scan_and_flip (pool->used_map, 0, bitmap_size(pool->used_map), false, page_cnt);
}

static size_t
next_fit_alloc (struct pool *pool, size_t page_cnt)
{
    struct bitmap *bm = pool->used_map;
    size_t end_idx = bitmap_size (bm);
    size_t page_idx = BITMAP_ERROR;
    
    size_t search_len = end_idx - pool->next_fit_last_idx;
    if (search_len > 0) {
        page_idx = bitmap_scan_and_flip (bm, pool->next_fit_last_idx, search_len, false, page_cnt);
    }

    if (page_idx == BITMAP_ERROR) {
        page_idx = bitmap_scan_and_flip (bm, 0, pool->next_fit_last_idx, false, page_cnt);
    }
    
    if (page_idx != BITMAP_ERROR) {
        pool->next_fit_last_idx = page_idx + page_cnt;
        if (pool->next_fit_last_idx >= end_idx) {
            pool->next_fit_last_idx = 0;
        }
    }
    
    return page_idx;
}

static size_t
best_fit_alloc (struct pool *pool, size_t page_cnt)
{
    struct bitmap *bm = pool->used_map;
    size_t end_idx = bitmap_size (bm);
    size_t best_idx = BITMAP_ERROR;
    size_t min_size = end_idx + 1; 
    size_t curr_idx = 0;

    while (curr_idx < end_idx) {
        size_t start = bitmap_scan (bm, curr_idx, end_idx - curr_idx, false, 1);
        
        if (start == BITMAP_ERROR) {
            break; 
        }

        size_t next_used = bitmap_scan (bm, start, end_idx - start, true, 1);
        size_t free_len;
        
        if (next_used == BITMAP_ERROR) {
            free_len = end_idx - start;
        } else {
            free_len = next_used - start;
        }

        if (free_len >= page_cnt) {
            if (free_len < min_size) {
                min_size = free_len;
                best_idx = start;
            }
            if (min_size == page_cnt) {
                break;
            }
        }
        
        curr_idx = start + free_len;
    }

    if (best_idx != BITMAP_ERROR) {
        bitmap_set_multiple(bm, best_idx, page_cnt, true);
        return best_idx;
    }
    
    return BITMAP_ERROR;
}

static size_t
buddy_system_alloc (struct pool *pool, size_t page_cnt)
{
    /* Buddy System 할당 로직 구현 필요 */
    return BITMAP_ERROR; 
}

static void
buddy_system_free (struct pool *pool, size_t page_idx, size_t page_cnt)
{
    /* Buddy System 해제 및 병합 로직 구현 필요 */
    bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
}
