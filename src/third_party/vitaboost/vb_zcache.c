/*
 * vb_zcache: LZ4-compressed ring cache. See include/vb_zcache.h.
 *
 * Layout: entries are placed in insertion order at increasing offsets of the
 * ring, wrapping once:
 *
 *   not wrapped:  [ free | oldest ... newest | free ]
 *   wrapped:      [ newer ... newest | free | oldest ... ]
 *
 * New data goes right after the newest entry (`head`); if it does not fit
 * before the end of the ring it goes to offset 0. Entries in the way are
 * evicted oldest first, which in both layouts are exactly the entries that
 * overlap the target range. Taken/erased entries stay in the order list as
 * dead placeholders until eviction reaches them, so `head` stays consistent.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "vb_zcache.h"
#include "lz4.h"

#define ZC_NONE 0xFFFFFFFFu
#define ZC_ALIGN 8u
#define ZC_PROBE_MIN (64u * 1024u)   /* blobs from this size get a quick probe */
#define ZC_PROBE_LEN (16u * 1024u)

typedef struct {
    uint64_t key;
    uint32_t off;
    uint32_t cap;     /* bytes reserved in the ring (aligned clen) */
    uint32_t clen;
    uint32_t rawlen;
    uint32_t older;   /* insertion order list (all placed entries) */
    uint32_t newer;
    uint32_t hnext;   /* hash chain (live) or free list */
    uint8_t  live;
    uint8_t  placed;
} zc_entry;

struct vb_zcache {
    uint8_t  *data;
    uint32_t  size;
    zc_entry *e;
    uint32_t  max_entries;
    uint32_t  free_slot;
    uint32_t *buckets;
    uint32_t  bucket_bits;
    uint32_t  oldest;
    uint32_t  newest;
    uint32_t  head;
    void     *lz4_state;
    uint8_t  *probe;
    unsigned  min_saving_pct;
    int       accel;
    vb_zcache_stats st;
};

static uint32_t align_up(uint32_t v) { return (v + ZC_ALIGN - 1) & ~(ZC_ALIGN - 1); }

static uint32_t bucket_of(const vb_zcache *zc, uint64_t key)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - zc->bucket_bits));
}

static uint32_t find(const vb_zcache *zc, uint64_t key)
{
    uint32_t i = zc->buckets[bucket_of(zc, key)];
    while (i != ZC_NONE && zc->e[i].key != key)
        i = zc->e[i].hnext;
    return i;
}

static void hash_remove(vb_zcache *zc, uint32_t idx)
{
    uint32_t *link = &zc->buckets[bucket_of(zc, zc->e[idx].key)];
    while (*link != ZC_NONE && *link != idx)
        link = &zc->e[*link].hnext;
    if (*link == idx)
        *link = zc->e[idx].hnext;
    zc->e[idx].hnext = ZC_NONE;
}

/* live -> dead placeholder */
static void kill(vb_zcache *zc, uint32_t idx)
{
    zc_entry *en = &zc->e[idx];
    if (!en->live)
        return;
    hash_remove(zc, idx);
    en->live = 0;
    zc->st.live_entries--;
    zc->st.live_bytes -= en->cap;
}

static void evict_oldest(vb_zcache *zc)
{
    uint32_t idx = zc->oldest;
    zc_entry *en = &zc->e[idx];
    if (en->live) {
        kill(zc, idx);
        zc->st.evictions++;
    }
    zc->oldest = en->newer;
    if (zc->oldest != ZC_NONE)
        zc->e[zc->oldest].older = ZC_NONE;
    else
        zc->newest = ZC_NONE;
    en->placed = 0;
    en->hnext = zc->free_slot;
    zc->free_slot = idx;
    if (zc->oldest == ZC_NONE)
        zc->head = 0;
}

static uint32_t alloc_slot(vb_zcache *zc)
{
    uint32_t idx;
    while (zc->free_slot == ZC_NONE && zc->oldest != ZC_NONE)
        evict_oldest(zc);
    idx = zc->free_slot;
    if (idx != ZC_NONE)
        zc->free_slot = zc->e[idx].hnext;
    return idx;
}

static void free_slot(vb_zcache *zc, uint32_t idx)
{
    zc->e[idx].hnext = zc->free_slot;
    zc->free_slot = idx;
}

static int overlaps(const zc_entry *en, uint32_t pos, uint32_t len)
{
    return en->off < pos + len && pos < en->off + en->cap;
}

/* Make [pos, pos+need) free and return pos. need <= size. */
static uint32_t make_room(vb_zcache *zc, uint32_t need)
{
    uint32_t pos;
    if (zc->oldest == ZC_NONE) {
        zc->head = 0;
        return 0;
    }
    pos = zc->head;
    if (pos + need > zc->size) {
        /* skip the tail gap: everything placed at or after head goes */
        while (zc->oldest != ZC_NONE && zc->e[zc->oldest].off >= zc->head)
            evict_oldest(zc);
        pos = 0;
    }
    while (zc->oldest != ZC_NONE && overlaps(&zc->e[zc->oldest], pos, need))
        evict_oldest(zc);
    return pos;
}

vb_zcache *vb_zcache_create(void *data, size_t size, uint32_t max_entries, unsigned min_saving_pct,
                            int acceleration)
{
    vb_zcache *zc;
    uint32_t i, nb;
    if (!data || size < 4096 || size > 0x7FFFFFFFu)
        return NULL;
    zc = calloc(1, sizeof(*zc));
    if (!zc)
        return NULL;
    zc->data = data;
    zc->size = (uint32_t)size & ~(ZC_ALIGN - 1);
    zc->max_entries = max_entries ? max_entries : (uint32_t)(size / 2048);
    if (zc->max_entries < 16)
        zc->max_entries = 16;
    zc->min_saving_pct = min_saving_pct > 90 ? 90 : min_saving_pct;
    zc->accel = acceleration < 1 ? 1 : acceleration;

    zc->bucket_bits = 4;
    while ((1u << zc->bucket_bits) < zc->max_entries && zc->bucket_bits < 24)
        zc->bucket_bits++;
    nb = 1u << zc->bucket_bits;

    zc->e = malloc(sizeof(zc_entry) * zc->max_entries);
    zc->buckets = malloc(sizeof(uint32_t) * nb);
    zc->lz4_state = malloc((size_t)LZ4_sizeofState());
    zc->probe = malloc((size_t)LZ4_compressBound((int)ZC_PROBE_LEN));
    if (!zc->e || !zc->buckets || !zc->lz4_state || !zc->probe) {
        vb_zcache_destroy(zc);
        return NULL;
    }
    for (i = 0; i < nb; i++)
        zc->buckets[i] = ZC_NONE;
    for (i = 0; i < zc->max_entries; i++) {
        memset(&zc->e[i], 0, sizeof(zc_entry));
        zc->e[i].hnext = (i + 1 < zc->max_entries) ? i + 1 : ZC_NONE;
    }
    zc->free_slot = 0;
    zc->oldest = zc->newest = ZC_NONE;
    zc->head = 0;
    zc->st.capacity = zc->size;
    return zc;
}

void vb_zcache_destroy(vb_zcache *zc)
{
    if (!zc)
        return;
    free(zc->probe);
    free(zc->lz4_state);
    free(zc->buckets);
    free(zc->e);
    free(zc);
}

int vb_zcache_put(vb_zcache *zc, uint64_t key, const void *src, uint32_t len)
{
    uint32_t limit, need, idx, pos;
    int clen;
    zc_entry *en;

    if (!zc || !src || len == 0 || len > 0x7FFFFFF0u)
        return -1;
    zc->st.puts++;
    vb_zcache_erase(zc, key);

    limit = (uint32_t)(((uint64_t)len * (100u - zc->min_saving_pct)) / 100u);
    if (limit < 16 || align_up(limit) > zc->size / 2) {
        zc->st.rejects++;
        return 1;
    }

    /* Big blobs: compress a slice first; block-compressed textures (DXT)
     * usually fail here cheaply instead of after a full pass. */
    if (len >= ZC_PROBE_MIN) {
        uint32_t plimit = (uint32_t)(((uint64_t)ZC_PROBE_LEN * (100u - zc->min_saving_pct)) / 100u);
        int pl = LZ4_compress_fast_extState(zc->lz4_state, (const char *)src, (char *)zc->probe,
                                            (int)ZC_PROBE_LEN, (int)plimit, zc->accel);
        if (pl <= 0) {
            zc->st.rejects++;
            return 1;
        }
    }

    need = align_up(limit);
    idx = alloc_slot(zc);
    if (idx == ZC_NONE) {
        zc->st.rejects++;
        return 1;
    }
    pos = make_room(zc, need);

    clen = LZ4_compress_fast_extState(zc->lz4_state, (const char *)src, (char *)zc->data + pos, (int)len,
                                      (int)limit, zc->accel);
    if (clen <= 0) {
        free_slot(zc, idx);
        zc->st.rejects++;
        return 1;
    }

    en = &zc->e[idx];
    en->key = key;
    en->off = pos;
    en->clen = (uint32_t)clen;
    en->cap = align_up((uint32_t)clen);
    en->rawlen = len;
    en->live = 1;
    en->placed = 1;
    en->newer = ZC_NONE;
    en->older = zc->newest;
    if (zc->newest != ZC_NONE)
        zc->e[zc->newest].newer = idx;
    else
        zc->oldest = idx;
    zc->newest = idx;
    zc->head = pos + en->cap;

    {
        uint32_t b = bucket_of(zc, key);
        en->hnext = zc->buckets[b];
        zc->buckets[b] = idx;
    }

    zc->st.stores++;
    zc->st.live_entries++;
    zc->st.live_bytes += en->cap;
    zc->st.raw_stored += len;
    zc->st.comp_stored += (uint32_t)clen;
    return 0;
}

int vb_zcache_take(vb_zcache *zc, uint64_t key, void *dst, uint32_t len)
{
    uint32_t idx;
    zc_entry *en;
    int r;
    if (!zc || !dst)
        return -1;
    idx = find(zc, key);
    if (idx == ZC_NONE) {
        zc->st.misses++;
        return -1;
    }
    en = &zc->e[idx];
    if (en->rawlen != len) {
        kill(zc, idx);
        zc->st.misses++;
        return -1;
    }
    r = LZ4_decompress_safe((const char *)zc->data + en->off, (char *)dst, (int)en->clen, (int)len);
    kill(zc, idx);
    if (r != (int)len) {
        zc->st.misses++;
        return -1;
    }
    zc->st.hits++;
    return 0;
}

void vb_zcache_erase(vb_zcache *zc, uint64_t key)
{
    uint32_t idx;
    if (!zc)
        return;
    idx = find(zc, key);
    if (idx != ZC_NONE)
        kill(zc, idx);
}

void vb_zcache_clear(vb_zcache *zc)
{
    if (!zc)
        return;
    while (zc->oldest != ZC_NONE) {
        if (zc->e[zc->oldest].live)
            kill(zc, zc->oldest);
        evict_oldest(zc);
    }
}

void vb_zcache_get_stats(const vb_zcache *zc, vb_zcache_stats *out)
{
    if (!out)
        return;
    if (!zc) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = zc->st;
}
