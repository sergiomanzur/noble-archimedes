/* Opt-in immutable command capture at the HLE boundary.
 *
 * The live renderer keeps executing synchronously on the guest thread; this
 * module only snapshots what a delayed worker would need, into preallocated
 * storage.  Bounded: overflow is explicit, never silent.  No per-command file
 * output; one CSV is written when the presented-flip window closes.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define RCP_ARENA_BYTES (48u << 20)
#define RCP_MAX_RECORDS 524288
#define RCP_MAX_TYPES 64
#define RCP_SNAPSHOT_CAP (128u << 10)

struct rcap_manifest_entry {
    uint32_t data;
    uint32_t resource_addr;
    uint32_t binds;
    uint32_t total;
    uint32_t valid;
    uint32_t fields[19];
};

struct rcap {
    struct rcap_manifest_entry manifest[128];
    uint32_t manifest_count;
    uint64_t manifest_bind_bytes;
    uint64_t manifest_unique_bytes;
    uint64_t texture_binds;
    uint64_t texture_null_binds;
    uint64_t renderer_content_bytes;
    uint64_t renderer_upload_bytes;
    uint64_t renderer_events;
    uint32_t texture_frame_first;
    uint32_t texture_frame_last;
    uint8_t arena[RCP_ARENA_BYTES];
    uint32_t used;
    uint32_t peak;
    uint32_t records;
    uint32_t overflow;
    uint64_t types[RCP_MAX_TYPES];
    uint64_t type_bytes[RCP_MAX_TYPES];
    uint64_t type_required[RCP_MAX_TYPES];
    uint64_t total_required;
    uint64_t cpu_ns;
    uint64_t sync_calls;
    uint64_t unsupported;
    const char *type_names[RCP_MAX_TYPES];
};

static struct rcap R;
static uint8_t rcap_scratch[4u << 20];

extern ptrdiff_t g_xbox_mem_offset;

static uint32_t rcap_type_id(struct rcap *c, const char *name)
{
    uint32_t i;
    for (i = 0; i < RCP_MAX_TYPES; i++) {
        if (c->type_names[i] && !strcmp(c->type_names[i], name))
            return i;
        if (!c->type_names[i]) {
            c->type_names[i] = name;
            return i;
        }
    }
    return RCP_MAX_TYPES - 1;
}

/* Core, testable: copies `size` bytes from `src` into the arena, adds a
 * record.  Returns 0 on success, 1 on overflow (explicit). */
int rcap_snapshot_core(struct rcap *c, const char *type, const void *src,
                       uint32_t size, uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t id = rcap_type_id(c, type);
    c->type_required[id] += size;
    c->total_required += size;
    if (c->records >= RCP_MAX_RECORDS
        || size > RCP_SNAPSHOT_CAP
        || c->used + size > RCP_ARENA_BYTES) {
        c->overflow++;
        return 1;
    }
    if (size)
        memcpy(c->arena + c->used, src, size);
    c->used += size;
    if (c->used > c->peak)
        c->peak = c->used;
    c->records++;
    c->types[id]++;
    c->type_bytes[id] += size;
    (void)a0; (void)a1; (void)a2;
    return 0;
}

/* ---- live wrappers ---- */

static int rcap_on;
static int rcap_inited;
static int rcap_active;
static uint32_t rcap_start_flip;
static uint32_t rcap_end_flip;
static char rcap_file[1024];
static int rcap_dumped;
static LARGE_INTEGER rcap_freq;

void recomp_render_capture_init(void)
{
    const char *value;
    const char *file;
    if (rcap_inited)
        return;
    rcap_inited = 1;
    value = getenv("RECOMP_RENDER_CAPTURE");
    if (!value || !*value || !strcmp(value, "0"))
        return;
    rcap_on = 1;
    QueryPerformanceFrequency(&rcap_freq);
    value = getenv("RECOMP_RENDER_CAPTURE_START");
    rcap_start_flip = value ? strtoul(value, NULL, 0) : 0;
    value = getenv("RECOMP_RENDER_CAPTURE_END");
    rcap_end_flip = value ? strtoul(value, NULL, 0) : 0;
    file = getenv("RECOMP_RENDER_CAPTURE_FILE");
    snprintf(rcap_file, sizeof(rcap_file), "%s",
             (file && *file) ? file : "render_capture.csv");
}

int recomp_render_capture_enabled(void)
{
    return rcap_on;
}

void recomp_render_capture_update_window(uint32_t flip)
{
    if (!rcap_on)
        return;
    if (rcap_start_flip && flip < rcap_start_flip) {
        rcap_active = 0;
        return;
    }
    if (rcap_end_flip && flip >= rcap_end_flip) {
        if (!rcap_dumped) {
            FILE *f = fopen(rcap_file, "w");
            uint32_t i;
            if (f) {
                fprintf(f, "type,count,cpu_ms,bytes_copied,bytes_required\n");
                for (i = 0; i < RCP_MAX_TYPES; i++) {
                    if (!R.type_names[i])
                        continue;
                    fprintf(f, "%s,%llu,%.6f,%llu,%llu\n",
                            R.type_names[i], R.types[i],
                            (double)R.cpu_ns / 1e6, R.type_bytes[i],
                            R.type_required[i]);
                }
                fprintf(f, "summary,peak_bytes,%u,,%llu\n",
                        R.peak, (unsigned long long)R.records);
                fprintf(f, "summary,required_bytes,,,%llu\n",
                        (unsigned long long)R.total_required);
                fprintf(f, "summary,overflow,%u,,%llu\n",
                        R.overflow, (unsigned long long)R.sync_calls);
                fprintf(f, "summary,unsupported,%llu,,\n",
                        (unsigned long long)R.unsupported);
                fclose(f);
            }
            {
                char mpath[1200];
                snprintf(mpath, sizeof(mpath), "%s.manifest.csv", rcap_file);
                f = fopen(mpath, "w");
                if (f) {
                    uint32_t j;
                    fprintf(f, "data,norm_addr,resource_addr,binds,total,valid,gpu_target,overflow,fmt,w,h,depth,levels,pitch,comp,cube,linear,face_bytes,raw0,raw1,raw2,raw3,raw4,raw5\n");
                    for (j = 0; j < R.manifest_count; j++) {
                        const struct rcap_manifest_entry *e = &R.manifest[j];
                        fprintf(f, "0x%08X,0x%08X,0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                                e->data, e->data | 0x80000000u, e->resource_addr,
                                e->binds, e->total, e->valid, e->fields[17],
                                e->fields[18], e->fields[6], e->fields[7],
                                e->fields[8], e->fields[9], e->fields[10],
                                e->fields[11], e->fields[12], e->fields[13],
                                e->fields[14], e->fields[15], e->fields[0],
                                e->fields[1], e->fields[2], e->fields[3],
                                e->fields[4], e->fields[5]);
                    }
                    fprintf(f, "unique_total,,,%llu\n", (unsigned long long)R.manifest_unique_bytes);
                    fprintf(f, "bind_total,,,%llu\n", (unsigned long long)R.manifest_bind_bytes);
                    fprintf(f, "texture_binds,,,,%llu\n", (unsigned long long)R.texture_binds);
                    fprintf(f, "texture_null_binds,,,,%llu\n", (unsigned long long)R.texture_null_binds);
                    fprintf(f, "renderer_events,,,,%llu\n", (unsigned long long)R.renderer_events);
                    fprintf(f, "renderer_content_bytes,,,,%llu\n", (unsigned long long)R.renderer_content_bytes);
                    fprintf(f, "renderer_upload_bytes,,,,%llu\n", (unsigned long long)R.renderer_upload_bytes);
                    fprintf(f, "texture_frame_range,,,,%u..%u\n", R.texture_frame_first, R.texture_frame_last);
                    fclose(f);
                }
            }
            rcap_dumped = 1;
        }
        rcap_on = 0;
        return;
    }
    rcap_active = 1;
}

static const void *rcap_ptr(uint32_t addr)
{
    return (const void *)((uintptr_t)(uint32_t)addr + (uintptr_t)g_xbox_mem_offset);
}

void recomp_rcap_snapshot(const char *type, uint32_t addr, uint32_t size,
                          uint32_t a0, uint32_t a1, uint32_t a2)
{
    LARGE_INTEGER a, b;
    if (!rcap_on || !rcap_active)
        return;
    QueryPerformanceCounter(&a);
    rcap_snapshot_core(&R, type, rcap_ptr(addr), size, a0, a1, a2);
    QueryPerformanceCounter(&b);
    R.cpu_ns += (uint64_t)((b.QuadPart - a.QuadPart) * 1000000000LL
                           / rcap_freq.QuadPart);
}

int recomp_rcap_active(void)
{
    return rcap_on && rcap_active;
}

void recomp_rcap_texture_frame(uint32_t frame)
{
    if (!rcap_on || !rcap_active)
        return;
    if (!R.texture_frame_first)
        R.texture_frame_first = frame;
    R.texture_frame_last = frame;
}

void recomp_rcap_renderer_bytes(uint64_t content, uint64_t upload)
{
    if (!rcap_on || !rcap_active)
        return;
    R.renderer_events++;
    R.renderer_content_bytes += content;
    R.renderer_upload_bytes += upload;
}

void recomp_rcap_texture_bind(uint32_t stage, uint32_t resource)
{
    (void)stage;
    if (!rcap_on || !rcap_active)
        return;
    R.texture_binds++;
    if (!resource)
        R.texture_null_binds++;
}

void recomp_rcap_manifest(const char *type, uint32_t addr, uint32_t total,
                          uint32_t valid, const uint32_t *fields,
                          uint32_t field_count)
{
    uint32_t i;
    struct rcap_manifest_entry *e = NULL;
    (void)type;
    if (!rcap_on || !rcap_active)
        return;
    for (i = 0; i < R.manifest_count; i++)
        if (R.manifest[i].data == fields[1] &&
            R.manifest[i].fields[3] == fields[3] &&
            R.manifest[i].fields[4] == fields[4]) {
            e = &R.manifest[i];
            break;
        }
    if (!e) {
        if (R.manifest_count >= 128) {
            R.unsupported++;
            return;
        }
        e = &R.manifest[R.manifest_count++];
        e->data = fields[1];
        e->resource_addr = addr;
        e->total = total;
        e->valid = valid;
        memcpy(e->fields, fields, field_count * sizeof(uint32_t));
        R.manifest_unique_bytes += total;
    }
    e->binds++;
    R.manifest_bind_bytes += total;
}

void recomp_rcap_require(const char *type, uint32_t size)
{
    uint32_t id;
    if (!rcap_on || !rcap_active)
        return;
    id = rcap_type_id(&R, type);
    R.types[id]++;
    R.type_required[id] += size;
    R.total_required += size;
}

void recomp_rcap_measure(const char *type, uint32_t addr, uint32_t size)
{
    uint32_t id;
    uint32_t offset;
    LARGE_INTEGER a, b;
    if (!rcap_on || !rcap_active)
        return;
    id = rcap_type_id(&R, type);
    R.types[id]++;
    R.type_required[id] += size;
    R.total_required += size;
    QueryPerformanceCounter(&a);
    for (offset = 0; offset < size; offset += sizeof(rcap_scratch)) {
        uint32_t chunk = size - offset;
        if (chunk > sizeof(rcap_scratch))
            chunk = sizeof(rcap_scratch);
        if (IsBadReadPtr(rcap_ptr(addr + offset), chunk)) {
            R.unsupported++;
            break;
        }
        memcpy(rcap_scratch, rcap_ptr(addr + offset), chunk);
    }
    QueryPerformanceCounter(&b);
    R.cpu_ns += (uint64_t)((b.QuadPart - a.QuadPart) * 1000000000LL
                           / rcap_freq.QuadPart);
}

void recomp_rcap_state(const char *type, uint32_t a0, uint32_t a1, uint32_t a2)
{
    if (!rcap_on || !rcap_active)
        return;
    R.types[rcap_type_id(&R, type)]++;
    R.records++;
}

void recomp_rcap_sync(const char *name)
{
    if (!rcap_on || !rcap_active)
        return;
    R.sync_calls++;
    R.types[rcap_type_id(&R, name)]++;
}

void recomp_rcap_unsupported(const char *name)
{
    if (!rcap_on || !rcap_active)
        return;
    R.unsupported++;
    R.types[rcap_type_id(&R, name)]++;
}
