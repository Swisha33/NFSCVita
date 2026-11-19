/*
 * vb_zcache - a small LZ4-compressed RAM cache for blobs that are expensive
 * to recreate (texture read-backs, decoded assets, ...).
 *
 * Standalone part of VitaBoost: needs only LZ4 and malloc, no kubridge, no
 * paging. Entries live in one caller-provided buffer used as a ring: new
 * entries are written at the head, the oldest are evicted to make room.
 * A hit removes the entry (take semantics), which fits caches whose user
 * gets the data back and re-inserts it later.
 *
 * NOT thread-safe: callers serialise access with their own lock.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VB_ZCACHE_H
#define VB_ZCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vb_zcache vb_zcache;

typedef struct vb_zcache_stats {
    uint32_t puts;          /* vb_zcache_put calls */
    uint32_t stores;        /* entries stored */
    uint32_t rejects;       /* did not compress enough (or too big) */
    uint32_t hits;          /* vb_zcache_take found and decompressed */
    uint32_t misses;
    uint32_t evictions;     /* live entries pushed out by new ones */
    uint64_t raw_stored;    /* uncompressed bytes of all stores */
    uint64_t comp_stored;   /* compressed bytes of all stores */
    uint32_t live_entries;
    uint32_t live_bytes;    /* compressed bytes currently held */
    uint32_t capacity;      /* ring size */
} vb_zcache_stats;

/* data/size: the ring (any CPU-accessible memory the caller owns, it is not
 *            freed by vb_zcache_destroy).
 * max_entries: metadata slots, 0 = size / 2048.
 * min_saving_pct: only keep blobs that shrink by at least this much (e.g. 20).
 * acceleration: LZ4 acceleration, 1 = best ratio, higher = faster (e.g. 4).
 * Metadata (~40 bytes per entry + 16 KiB LZ4 state) comes from malloc. */
vb_zcache *vb_zcache_create(void *data, size_t size, uint32_t max_entries, unsigned min_saving_pct,
                            int acceleration);
void vb_zcache_destroy(vb_zcache *zc);

/* Compress and store len bytes under key (replaces an older entry with the
 * same key). Returns 0 if stored, 1 if rejected (did not compress well
 * enough, or too big), -1 on bad arguments. */
int vb_zcache_put(vb_zcache *zc, uint64_t key, const void *src, uint32_t len);

/* If key is cached with exactly len uncompressed bytes, decompress into dst,
 * remove the entry and return 0. Otherwise return -1 (dst untouched unless
 * the entry was corrupt). */
int vb_zcache_take(vb_zcache *zc, uint64_t key, void *dst, uint32_t len);

/* Drop key if present. */
void vb_zcache_erase(vb_zcache *zc, uint64_t key);

/* Drop everything. */
void vb_zcache_clear(vb_zcache *zc);

void vb_zcache_get_stats(const vb_zcache *zc, vb_zcache_stats *out);

#ifdef __cplusplus
}
#endif

#endif
