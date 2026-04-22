#include "buddy.h"
#include <string.h>

#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096

/* Memory pool metadata */
static void *base_addr = NULL;
static int total_pages = 0;

/* rank_info[i] = rank of allocated block that page i belongs to (0 if free) */
#define MAX_PAGE_COUNT 131072
static int rank_info[MAX_PAGE_COUNT];

/* Bitmap-based free lists.
 * For rank r, the bitmap has (total_pages >> (r-1)) bits.
 * Bit i = 1 means the i-th block of rank r is free. */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITMAP_WORDS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static unsigned long free_bitmap[MAX_RANK + 1][BITMAP_WORDS(MAX_PAGE_COUNT)];

/* Helper: get page index from address */
static int addr_to_idx(void *p) {
    if (p < base_addr) return -1;
    long offset = (char *)p - (char *)base_addr;
    if (offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

/* Helper: compute block size in pages for a rank */
static int rank_size(int rank) {
    return 1 << (rank - 1);
}

/* Helper: set or clear a bit in the free bitmap */
static void set_free_bit(int rank, int block_idx, int free) {
    int word = block_idx / BITS_PER_LONG;
    int bit = block_idx % BITS_PER_LONG;
    if (free)
        free_bitmap[rank][word] |= (1UL << bit);
    else
        free_bitmap[rank][word] &= ~(1UL << bit);
}

/* Helper: test a bit in the free bitmap */
static int test_free_bit(int rank, int block_idx) {
    int word = block_idx / BITS_PER_LONG;
    int bit = block_idx % BITS_PER_LONG;
    return (free_bitmap[rank][word] >> bit) & 1UL;
}

/* Helper: find first free block of given rank (returns -1 if none) */
static int find_first_free(int rank) {
    int nwords = BITMAP_WORDS(total_pages >> (rank - 1));
    int i;
    for (i = 0; i < nwords; i++) {
        if (free_bitmap[rank][i] != 0) {
            int bit = __builtin_ctzl(free_bitmap[rank][i]);
            return i * BITS_PER_LONG + bit;
        }
    }
    return -1;
}

int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;

    memset(rank_info, 0, pgcount * sizeof(int));
    memset(free_bitmap, 0, sizeof(free_bitmap));

    /* Find the largest rank that fits in pgcount pages */
    int max_rank = 0;
    int i;
    for (i = 1; i <= MAX_RANK; i++) {
        if (rank_size(i) <= pgcount)
            max_rank = i;
        else
            break;
    }

    /* Mark the entire pool as one free block of max_rank */
    if (max_rank >= 1) {
        set_free_bit(max_rank, 0, 1);
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK)
        return (void *)(long)-EINVAL;

    /* Find smallest available block >= requested rank */
    int i;
    for (i = rank; i <= MAX_RANK; i++) {
        int block_idx = find_first_free(i);
        if (block_idx >= 0) {
            /* Remove this block from free list */
            set_free_bit(i, block_idx, 0);

            /* Split until we get to desired rank */
            while (i > rank) {
                i--;
                /* Split into two buddies: first = block_idx*2, second = block_idx*2+1 */
                int second = block_idx * 2 + 1;
                set_free_bit(i, second, 1);
                block_idx = block_idx * 2;
            }

            /* Mark pages as allocated */
            int page_idx = block_idx * rank_size(rank);
            int size = rank_size(rank);
            int j;
            for (j = 0; j < size; j++) {
                rank_info[page_idx + j] = rank;
            }

            return (void *)((char *)base_addr + page_idx * PAGE_SIZE);
        }
    }

    return (void *)(long)-ENOSPC;
}

int return_pages(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = addr_to_idx(p);
    if (idx < 0) return -EINVAL;

    int r = rank_info[idx];
    if (r == 0) return -EINVAL;

    int size = rank_size(r);

    /* Verify this is the start of an allocated block */
    if (idx % size != 0) return -EINVAL;

    /* Verify all pages in this block have the same rank */
    int j;
    for (j = 0; j < size; j++) {
        if (idx + j >= total_pages || rank_info[idx + j] != r)
            return -EINVAL;
    }

    /* Mark as free */
    for (j = 0; j < size; j++) {
        rank_info[idx + j] = 0;
    }

    /* Try to merge with buddy */
    int current_rank = r;
    int block_idx = idx / rank_size(current_rank);

    while (current_rank < MAX_RANK) {
        int buddy_block_idx = block_idx ^ 1;

        /* Check if buddy is within bounds */
        int buddy_page = buddy_block_idx * rank_size(current_rank);
        if (buddy_page + rank_size(current_rank) > total_pages)
            break;

        /* Check if buddy is free */
        if (!test_free_bit(current_rank, buddy_block_idx))
            break;

        /* Buddy is free — merge. Clear buddy's free bit. */
        set_free_bit(current_rank, buddy_block_idx, 0);

        /* The merged block's block_idx at the next rank */
        block_idx = block_idx / 2;
        current_rank++;
    }

    /* Mark the merged block as free */
    set_free_bit(current_rank, block_idx, 1);

    return OK;
}

int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = addr_to_idx(p);
    if (idx < 0) return -EINVAL;

    /* Check if allocated */
    if (rank_info[idx] != 0) {
        int r = rank_info[idx];
        if (idx % rank_size(r) == 0)
            return r;
        return -EINVAL;
    }

    /* Free block: find its maximum rank by checking bitmaps */
    int r;
    for (r = MAX_RANK; r >= 1; r--) {
        int size = rank_size(r);
        if (idx % size != 0) continue;
        int block_idx = idx / size;
        if (test_free_bit(r, block_idx))
            return r;
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    int count = 0;
    int nbits = total_pages >> (rank - 1);
    int nwords = BITMAP_WORDS(nbits);
    int i;
    for (i = 0; i < nwords; i++) {
        count += __builtin_popcountl(free_bitmap[rank][i]);
    }
    return count;
}
