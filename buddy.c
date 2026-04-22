#include "buddy.h"

#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096

/* Memory pool metadata */
static void *base_addr = NULL;
static int total_pages = 0;

/* Free list heads for each rank (1..MAX_RANK) */
static struct free_block {
    struct free_block *next;
} *free_lists[MAX_RANK + 1];

/* rank_info[i] = rank of the block that page i belongs to (0 if free).
 * Maximum pgcount is MAXRANK0PAGE = 32768 (from main.c). */
#define MAX_PAGE_COUNT 131072
static int rank_info[MAX_PAGE_COUNT];

/* Helper: get page index from address */
static int addr_to_idx(void *p) {
    if (p < base_addr) return -1;
    long offset = (char *)p - (char *)base_addr;
    if (offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

/* Helper: get address from page index */
static void *idx_to_addr(int idx) {
    return (char *)base_addr + idx * PAGE_SIZE;
}

/* Helper: find the buddy of a block at given index with given rank */
static int buddy_idx(int idx, int rank) {
    int block_size = 1 << (rank - 1);  /* number of pages for this rank */
    return idx ^ block_size;
}

int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;

    int i;
    for (i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }
    for (i = 0; i < pgcount; i++) {
        rank_info[i] = 0;
    }

    /* Find the largest rank that fits in pgcount pages */
    int max_rank = 0;
    int size = 1;
    for (i = 1; i <= MAX_RANK; i++) {
        if (size <= pgcount) {
            max_rank = i;
        } else {
            break;
        }
        size <<= 1;
    }

    /* Add the entire pool as one big free block of max_rank */
    if (max_rank >= 1) {
        struct free_block *block = (struct free_block *)p;
        block->next = NULL;
        free_lists[max_rank] = block;
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK)
        return (void *)(long)-EINVAL;

    int i;
    /* Find smallest available block >= requested rank */
    for (i = rank; i <= MAX_RANK; i++) {
        if (free_lists[i] != NULL)
            break;
    }

    if (i > MAX_RANK)
        return (void *)(long)-ENOSPC;

    /* Remove block from free list of rank i */
    struct free_block *block = free_lists[i];
    free_lists[i] = block->next;

    /* Split until we get to desired rank */
    while (i > rank) {
        i--;
        int block_size_pages = 1 << (i - 1);
        /* The buddy is the second half of the block */
        struct free_block *buddy = (struct free_block *)((char *)block + block_size_pages * PAGE_SIZE);
        buddy->next = free_lists[i];
        free_lists[i] = buddy;
    }

    /* Mark pages as allocated with this rank */
    int idx = addr_to_idx(block);
    int block_size = 1 << (rank - 1);
    int j;
    for (j = 0; j < block_size; j++) {
        rank_info[idx + j] = rank;
    }

    return block;
}

int return_pages(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = addr_to_idx(p);
    if (idx < 0) return -EINVAL;

    int r = rank_info[idx];
    if (r == 0) return -EINVAL;  /* Not allocated */

    int block_size = 1 << (r - 1);

    /* Verify this is the start of an allocated block */
    if (idx % block_size != 0) return -EINVAL;

    /* Verify all pages in this block have the same rank */
    int j;
    for (j = 0; j < block_size; j++) {
        if (idx + j >= total_pages || rank_info[idx + j] != r)
            return -EINVAL;
    }

    /* Mark as free */
    for (j = 0; j < block_size; j++) {
        rank_info[idx + j] = 0;
    }

    /* Try to merge with buddy */
    struct free_block *block = (struct free_block *)p;
    int current_rank = r;

    while (current_rank < MAX_RANK) {
        int buddy = buddy_idx(idx, current_rank);
        int buddy_block_size = 1 << (current_rank - 1);

        /* Check if buddy is within bounds and all its pages are free */
        if (buddy + buddy_block_size > total_pages)
            break;

        int buddy_free = 1;
        int k;
        for (k = 0; k < buddy_block_size; k++) {
            if (rank_info[buddy + k] != 0) {
                buddy_free = 0;
                break;
            }
        }

        if (!buddy_free) break;

        /* Remove buddy from its free list */
        struct free_block **prev = &free_lists[current_rank];
        struct free_block *curr = free_lists[current_rank];
        while (curr) {
            if (curr == (struct free_block *)idx_to_addr(buddy)) {
                *prev = curr->next;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        /* Merge: the merged block starts at the lower address */
        if (buddy < idx) {
            idx = buddy;
            block = (struct free_block *)idx_to_addr(idx);
        }

        current_rank++;
    }

    /* Add merged block to free list */
    block->next = free_lists[current_rank];
    free_lists[current_rank] = block;

    return OK;
}

int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = addr_to_idx(p);
    if (idx < 0) return -EINVAL;

    /* Check if allocated */
    if (rank_info[idx] != 0) {
        int r = rank_info[idx];
        int block_size = 1 << (r - 1);
        if (idx % block_size == 0)
            return r;
        return -EINVAL;
    }

    /* Free block: find its maximum rank by scanning free lists */
    int r;
    for (r = MAX_RANK; r >= 1; r--) {
        int block_size = 1 << (r - 1);
        if (idx % block_size != 0) continue;
        struct free_block *fb;
        for (fb = free_lists[r]; fb; fb = fb->next) {
            if ((struct free_block *)idx_to_addr(idx) == fb)
                return r;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    int count = 0;
    struct free_block *fb;
    for (fb = free_lists[rank]; fb; fb = fb->next) {
        count++;
    }
    return count;
}
