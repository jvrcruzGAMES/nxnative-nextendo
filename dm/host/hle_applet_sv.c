// Applet services (appletAE, appletOE, fatal:u) for library applets (Mii Maker, software keyboard, etc.)
#include "hle.h"
#include "front.h"
#include "hle_mii.h"
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    uint8_t data[0x1000];
    size_t size;
} storage_ctx;

static storage_ctx *g_last_written_storage = NULL;
static dm_applet_shm_t *applet_shm_map(void);

static Result storage_accessor_answer(sf_req *q) {
    storage_ctx *ctx = (storage_ctx *)q->ctx;
    if (q->cmd == 0) { // GetSize
        q->out_len = 8;
        *(uint64_t *)q->out = ctx ? ctx->size : 0;
        return RES_OK;
    }
    if (q->cmd == 10) { // Write(offset)
        uint64_t off = q->in_len >= 8 ? *(uint64_t *)q->in : 0;
        if (ctx && q->na > 0 && q->a[0].p && q->a[0].size > 0) {
            size_t copy_sz = q->a[0].size;
            if (off + copy_sz > sizeof ctx->data) copy_sz = sizeof ctx->data > off ? sizeof ctx->data - off : 0;
            memcpy(ctx->data + off, q->a[0].p, copy_sz);
            if (off + copy_sz > ctx->size) ctx->size = off + copy_sz;
            g_last_written_storage = ctx;
        }
        return RES_OK;
    }
    if (q->cmd == 11) { // Read(offset)
        uint64_t off = q->in_len >= 8 ? *(uint64_t *)q->in : 0;
        if (ctx && q->nb > 0 && q->b[0].p && q->b[0].size > 0) {
            size_t copy_sz = q->b[0].size;
            if (off >= ctx->size) copy_sz = 0;
            else if (off + copy_sz > ctx->size) copy_sz = ctx->size - off;
            if (copy_sz) memcpy(q->b[0].p, ctx->data + off, copy_sz);
        }
        return RES_OK;
    }
    return RES_OK;
}

static Result storage_answer(sf_req *q) {
    if (q->cmd == 0) { // Open -> IStorageAccessor
        q->out_move = sf_session("IStorageAccessor", storage_accessor_answer, q->ctx, NULL);
        return RES_OK;
    }
    return RES_OK;
}

static uint32_t g_csg_event_w = 0;
static int g_csg_messages[4] = { 1, 0, 0, 0 }; // Message 1: Focus / ChangeIntoForeground
static int g_csg_num_messages = 1;
static int g_csg_msg_idx = 0;

static Result common_state_getter_answer(sf_req *q) {
    if (q->cmd == 0) { // GetEventHandle
        uint32_t ev_w = 0;
        uint32_t ev_r = hle_new_event(&ev_w);
        g_csg_event_w = ev_w;
        if (g_csg_msg_idx < g_csg_num_messages) {
            hle_signal_event_handle(ev_w);
        }
        q->out_copy = ev_r;
        LOG("sf: ICommonStateGetter GetEventHandle -> ev_r=%u ev_w=%u (signaled=%d)",
            ev_r, ev_w, g_csg_msg_idx < g_csg_num_messages);
        return RES_OK;
    }
    if (q->cmd == 1) { // ReceiveMessage
        q->out_len = 4;
        if (g_csg_msg_idx < g_csg_num_messages) {
            uint32_t msg = g_csg_messages[g_csg_msg_idx++];
            *(uint32_t *)q->out = msg;
            LOG("sf: ICommonStateGetter ReceiveMessage -> %u", msg);
            if (g_csg_msg_idx < g_csg_num_messages && g_csg_event_w) {
                hle_signal_event_handle(g_csg_event_w);
            }
            return RES_OK;
        }
        *(uint32_t *)q->out = 0;
        LOG("sf: ICommonStateGetter ReceiveMessage -> no messages (0x680)");
        return 0x680; // 0x680: nn::am::ResultNoMessage (module 128, desc 3)
    }
    if (q->cmd == 5) { // GetOperationMode
        q->out_len = 1;
        q->out[0] = (uint8_t)dm_opmode();
        return RES_OK;
    }
    if (q->cmd == 6) { // GetPerformanceMode
        q->out_len = 4;
        *(uint32_t *)q->out = 0; // Normal performance
        return RES_OK;
    }
    if (q->cmd == 9) { // GetCurrentFocusState
        q->out_len = 1;
        q->out[0] = 1; // InFocus
        return RES_OK;
    }
    return RES_OK;
}

static Result self_controller_answer(sf_req *q) {
    if (q->cmd == 9) { // GetLibraryAppletLaunchableEvent: always signaled so CreateLibraryApplet (e.g. swkbd) never blocks
        uint32_t ev_w = 0;
        uint32_t ev_r = hle_new_event(&ev_w);
        hle_signal_event_handle(ev_w);
        q->out_copy = ev_r;
        return RES_OK;
    }
    if (q->cmd == 18) { // CreateManagedDisplayLayer
        q->out_len = 8;
        *(uint64_t *)q->out = 1;
        return RES_OK;
    }
    if (q->cmd == 41) { // GetManagedDisplayLayerSeparationMode: return non-zero so applets use standard nvnWindow instead of SharedLayer (nvnMemoryPoolBuilderSetNativeHandleNVX)
        return 0x680;
    }
    if (q->cmd == 44) { // CreateManagedDisplaySeparableLayer
        q->out_len = 16;
        uint64_t *out = (uint64_t *)q->out;
        out[0] = 1; // display layer id
        out[1] = 2; // recording layer id
        return RES_OK;
    }
    return RES_OK;
}

// Child library applet creation (ILibraryAppletCreator / ILibraryAppletAccessor): when an applet or game creates AppletId 0x11 (Swkbd / software keyboard),
// Start invokes the host's native Apple Input modal (swkbd_show + front_text_input) instead of launching the Nintendo keyboard applet
extern void *hle_get_tmem(uint32_t h, size_t *size);
typedef int (*swkbd_input_fn)(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
extern Result swkbd_show(void *out, const void *arg, swkbd_input_fn input);
extern int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);

static storage_ctx *g_recent_storages[8];
static int g_num_recent_storages = 0;
static void * g_recent_tmem_ptr = NULL;
static size_t g_recent_tmem_sz = 0;

typedef struct {
    uint32_t applet_id;
    uint32_t mode;
    uint32_t state_ev_w;
    uint32_t out_ev_w;
    storage_ctx *out_storage;
} applet_accessor_ctx;

static Result library_applet_accessor_answer(sf_req *q) {
    applet_accessor_ctx *ac = (applet_accessor_ctx *)q->ctx;
    if (q->cmd == 0) { // GetAppletStateChangedEvent
        uint32_t ev_w = 0;
        uint32_t ev_r = hle_new_event(&ev_w);
        if (ac) ac->state_ev_w = ev_w;
        q->out_copy = ev_r;
        return RES_OK;
    }
    if (q->cmd == 10) { // Start
        if (ac && ac->applet_id == 0x11) { // AppletId_swkbd -> Apple Input modal
            storage_ctx *cfg_st = NULL;
            for (int i = g_num_recent_storages - 1; i >= 0; i--) {
                if (g_recent_storages[i] && g_recent_storages[i]->size >= 0x400) {
                    cfg_st = g_recent_storages[i];
                    break;
                }
            }
            uint8_t arg_buf[0x500];
            memset(arg_buf, 0, sizeof arg_buf);
            if (cfg_st) {
                memcpy(arg_buf, cfg_st->data, cfg_st->size < 0x4c8 ? cfg_st->size : 0x4c8);
            }
            memcpy(arg_buf + 0x4d0, &g_recent_tmem_ptr, 8);
            memcpy(arg_buf + 0x4d8, &g_recent_tmem_sz, 8);

            storage_ctx *out_st = calloc(1, sizeof *out_st);
            out_st->size = 0x7d4;
            struct { void *buf; uint64_t cap; } str_out = { out_st->data + 4, 0x7d0 };
            Result r = swkbd_show(&str_out, arg_buf, front_text_input);
            *(uint32_t *)(out_st->data + 0) = (r == RES_OK) ? 0u : 1u; // CloseResult: 0 = OK, 1 = Cancel
            ac->out_storage = out_st;
            g_num_recent_storages = 0;
            g_recent_tmem_ptr = NULL;
            g_recent_tmem_sz = 0;
            LOG("sf: ILibraryAppletAccessor Start(Swkbd 0x11) -> Apple Input modal result=%#x", r);
        } else if (ac && ac->applet_id == 0x16) { // MiiEdit
            uint8_t dummy_charinfo[0x58] = {0};
            uint32_t mode = 0;
            for (int i = 0; i < g_num_recent_storages; i++) {
                storage_ctx *st = g_recent_storages[i];
                if (st && st->size >= 8) { mode = *(const uint32_t *)(st->data + 4); break; }
            }
            int is_append = (mode == 1) ? 1 : 0;
            dm_mii_applet_prepare(dummy_charinfo, is_append);
            int saved = front_mii_editor(dummy_charinfo, is_append);
            int front_mii_editor_last_index(void);
            int out_idx = dm_mii_applet_commit(dummy_charinfo, saved, is_append, front_mii_editor_last_index());
            storage_ctx *out_st = calloc(1, sizeof *out_st);
            out_st->size = 0x10;
            *(uint32_t *)(out_st->data + 0) = (saved > 0) ? 0u : 1u; // 0 = Success, 1 = Canceled
            *(int32_t *)(out_st->data + 4) = out_idx >= 0 ? out_idx : 0;
            ac->out_storage = out_st;
            g_num_recent_storages = 0;
            LOG("sf: ILibraryAppletAccessor Start(MiiEdit 0x16) -> saved=%d index=%d", saved, out_idx);
        }
        if (ac && ac->out_ev_w) hle_signal_event_handle(ac->out_ev_w);
        if (ac && ac->state_ev_w) hle_signal_event_handle(ac->state_ev_w);
        return RES_OK;
    }
    if (q->cmd == 20 || q->cmd == 25 || q->cmd == 30) { // RequestExit / Terminate / GetResult
        return RES_OK;
    }
    if (q->cmd == 100 || q->cmd == 102) { // PushInData / PushInteractiveInData
        return RES_OK;
    }
    if (q->cmd == 101) { // PopOutData -> IStorage
        storage_ctx *st = (ac && ac->out_storage) ? ac->out_storage : NULL;
        if (ac) ac->out_storage = NULL;
        if (!st) {
            st = calloc(1, sizeof *st);
            st->size = 0x20;
        }
        q->out_move = sf_session("IStorage", storage_answer, st, free);
        return RES_OK;
    }
    if (q->cmd == 103) { // PopInteractiveOutData
        return 0x480; // No data
    }
    if (q->cmd == 105) { // GetPopOutDataEvent
        uint32_t ev_w = 0;
        uint32_t ev_r = hle_new_event(&ev_w);
        if (ac) ac->out_ev_w = ev_w;
        q->out_copy = ev_r;
        return RES_OK;
    }
    if (q->cmd == 106) { // GetPopInteractiveOutDataEvent
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    if (q->cmd == 160) { // GetIndirectLayerConsumerHandle
        q->out_len = 8;
        *(uint64_t *)q->out = 1;
        return RES_OK;
    }
    return RES_OK;
}

static Result library_applet_creator_answer(sf_req *q) {
    if (q->cmd == 0) { // CreateLibraryApplet(u32 applet_id, u32 mode) -> ILibraryAppletAccessor
        applet_accessor_ctx *ac = calloc(1, sizeof *ac);
        ac->applet_id = q->in_len >= 4 ? *(uint32_t *)q->in : 0;
        ac->mode = q->in_len >= 8 ? *(uint32_t *)(q->in + 4) : 0;
        g_num_recent_storages = 0;
        g_recent_tmem_ptr = NULL;
        g_recent_tmem_sz = 0;
        q->out_move = sf_session("ILibraryAppletAccessor", library_applet_accessor_answer, ac, free);
        LOG("sf: ILibraryAppletCreator CreateLibraryApplet(id=%#x, mode=%u)", ac->applet_id, ac->mode);
        return RES_OK;
    }
    if (q->cmd == 10) { // CreateStorage(u64 size) -> IStorage
        uint64_t sz = q->in_len >= 8 ? *(uint64_t *)q->in : 0x100;
        storage_ctx *st = calloc(1, sizeof *st);
        st->size = sz < sizeof st->data ? (size_t)sz : sizeof st->data;
        if (g_num_recent_storages < 8) g_recent_storages[g_num_recent_storages++] = st;
        q->out_move = sf_session("IStorage", storage_answer, st, NULL);
        return RES_OK;
    }
    if (q->cmd == 11) { // CreateTransferMemoryStorage(bool writable, u64 size, handle tmem) -> IStorage
        size_t tsz = 0;
        void *tptr = (q->ncopy > 0) ? hle_get_tmem(q->copy[0], &tsz) : NULL;
        if (tptr) {
            g_recent_tmem_ptr = tptr;
            g_recent_tmem_sz = tsz;
        }
        storage_ctx *st = calloc(1, sizeof *st);
        if (tptr && tsz > 0) {
            size_t cp = tsz < sizeof st->data ? tsz : sizeof st->data;
            memcpy(st->data, tptr, cp);
            st->size = cp;
        }
        q->out_move = sf_session("IStorage", storage_answer, st, free);
        return RES_OK;
    }
    if (q->cmd == 12) { // CreateHandleStorage(u64 size, handle) -> IStorage
        storage_ctx *st = calloc(1, sizeof *st);
        st->size = 0x100;
        q->out_move = sf_session("IStorage", storage_answer, st, free);
        return RES_OK;
    }
    return RES_OK;
}

static Result window_controller_answer(sf_req *q) {
    if (q->cmd == 1) { // GetAppletResourceUserId
        q->out_len = 8;
        *(uint64_t *)q->out = 1;
        return RES_OK;
    }
    return RES_OK;
}

static Result library_applet_self_accessor_answer(sf_req *q) {
    if (q->cmd == 0) { // PopInData -> IStorage
        storage_ctx *st = calloc(1, sizeof *st);
        st->size = 0x100;
        const char *mode_env = getenv("DM_MIIEDIT_MODE");
        uint32_t mode = (mode_env && *mode_env) ? (uint32_t)atoi(mode_env) : 0; // 0 = ShowMiiEdit, 1 = AppendMii
        *(uint32_t *)(st->data + 0x00) = 4;          // version (<= 4)
        *(uint32_t *)(st->data + 0x04) = mode;       // AppletMode: 0 = ShowMiiEdit, 1 = AppendMii
        *(uint32_t *)(st->data + 0x08) = 0xa523b78f; // Special Mii key code expected at x19+0xc (st->data+0x08)
        *(uint32_t *)(st->data + 0x0c) = 0xa523b78f;
        q->out_move = sf_session("IStorage", storage_answer, st, free);
        LOG("sf: ILibraryAppletSelfAccessor PopInData -> input storage (mode=%u, magic=0xa523b78f)", mode);
        return RES_OK;
    }
    if (q->cmd == 1) { // PushOutData
        storage_ctx *st = g_last_written_storage;
        dm_applet_shm_t *shm = applet_shm_map();
        if (st && st->size >= 8) {
            int32_t res = *(const int32_t *)(st->data + 0);
            int32_t idx = *(const int32_t *)(st->data + 4);
            LOG("sf: ILibraryAppletSelfAccessor PushOutData (size=%zu, result=%d, index=%d)", st->size, res, idx);
            if (shm) {
                atomic_store_explicit(&shm->out_result, res, memory_order_relaxed);
                atomic_store_explicit(&shm->out_index, idx, memory_order_relaxed);
                atomic_store_explicit(&shm->out_ready, 1u, memory_order_release);
            }
        } else {
            LOG("sf: ILibraryAppletSelfAccessor PushOutData (no storage data)");
        }
        return RES_OK;
    }
    if (q->cmd == 5) { // GetPopInDataEvent
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    if (q->cmd == 6) { // GetPopInteractiveInDataEvent
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    if (q->cmd == 10) { // ExitProcessAndReturn
        LOG("sf: ILibraryAppletSelfAccessor ExitProcessAndReturn: applet requested exit");
        exit(0);
        return RES_OK;
    }
    if (q->cmd == 11) { // GetLibraryAppletInfo
        q->out_len = 8;
        uint32_t *info = (uint32_t *)q->out;
        info[0] = 0x10; // AppletId
        info[1] = 1;    // LibraryAppletVersion
        return RES_OK;
    }
    if (q->cmd == 14) { // GetDesiredLanguage
        q->out_len = 8;
        *(uint64_t *)q->out = 1; // American English
        return RES_OK;
    }
    return RES_OK;
}

static Result library_applet_proxy_answer(sf_req *q) {
    if (q->cmd == 0) { // GetCommonStateGetter
        q->out_move = sf_session("ICommonStateGetter", common_state_getter_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 1) { // GetSelfController
        q->out_move = sf_session("ISelfController", self_controller_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 2) { // GetWindowController
        q->out_move = sf_session("IWindowController", window_controller_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 3) { // GetAudioController
        q->out_move = sf_session("IAudioController", NULL, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 4) { // GetDisplayController
        q->out_move = sf_session("IDisplayController", NULL, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 10) { // GetProcessWindingController
        q->out_move = sf_session("IProcessWindingController", NULL, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 11) { // GetLibraryAppletCreator
        q->out_move = sf_session("ILibraryAppletCreator", library_applet_creator_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 20) { // GetLibraryAppletSelfAccessor
        q->out_move = sf_session("ILibraryAppletSelfAccessor", library_applet_self_accessor_answer, NULL, NULL);
        return RES_OK;
    }
    return RES_OK;
}

static Result applet_ae_answer(sf_req *q) {
    if (q->cmd == 100 || q->cmd == 200 || q->cmd == 201 || q->cmd == 300 || q->cmd == 350) {
        q->out_move = sf_session("ILibraryAppletProxy", library_applet_proxy_answer, NULL, NULL);
        LOG("sf: appletAE/appletOE command %u -> ILibraryAppletProxy", q->cmd);
        return RES_OK;
    }
    return RES_OK;
}
static Result applet_oe_answer(sf_req *q) {
    return applet_ae_answer(q);
}
SF_SERVICE("appletAE", applet_ae_answer);
SF_SERVICE("appletOE", applet_oe_answer);

static Result fatal_u_answer(sf_req *q) {
    LOG("[RED] sf: fatal:u called (cmd=%u)", q->cmd);
    return RES_OK;
}
SF_SERVICE("fatal:u", fatal_u_answer);

static Result prepo_answer(sf_req *q) {
    if (q->cmd == 10300) { // GetTransmissionStatus -> u32 status = 0 (idle)
        uint32_t st = 0;
        memcpy(q->out, &st, sizeof(st));
        q->out_len = sizeof(st);
        return RES_OK;
    }
    if (q->cmd == 20100) { // GetSystemSessionId -> u64 session_id = 1
        uint64_t sid = 1;
        memcpy(q->out, &sid, sizeof(sid));
        q->out_len = sizeof(sid);
        return RES_OK;
    }
    return RES_OK;
}
static Result prepo_a_answer(sf_req *q) { return prepo_answer(q); }
static Result prepo_a2_answer(sf_req *q) { return prepo_answer(q); }
static Result prepo_m_answer(sf_req *q) { return prepo_answer(q); }
static Result prepo_s_answer(sf_req *q) { return prepo_answer(q); }
SF_SERVICE("prepo:u", prepo_answer);
SF_SERVICE("prepo:a", prepo_a_answer);
SF_SERVICE("prepo:a2", prepo_a2_answer);
SF_SERVICE("prepo:m", prepo_m_answer);
SF_SERVICE("prepo:s", prepo_s_answer);

typedef struct {
    int fd;
    size_t size;
    uint8_t *mapped;
} fsp_storage_ctx;

static fsp_storage_ctx *g_rom_storage = NULL;
static fsp_storage_ctx *get_rom_storage(void);

typedef struct {
    uint64_t tid;
    fsp_storage_ctx ctx;
} sysdata_storage_entry;
static sysdata_storage_entry g_sysdata_cache[16];
static int g_num_sysdata = 0;

static fsp_storage_ctx *get_sysdata_storage(uint64_t tid) {
    for (int i = 0; i < g_num_sysdata; i++) {
        if (g_sysdata_cache[i].tid == tid) return &g_sysdata_cache[i].ctx;
    }
    char path[1024];
    const char *env_sys = getenv("DM_SYSTEM_DIR");
    const char *env_root = getenv("DM_ROOT");
    const char *prefixes[10] = {0};
    int np = 0;
    char sys_sub[1024], root_sub[1024];
    if (env_sys && *env_sys) {
        prefixes[np++] = env_sys;
        snprintf(sys_sub, sizeof sys_sub, "%s/sysdata", env_sys);
        prefixes[np++] = sys_sub;
    }
    if (env_root && *env_root) {
        snprintf(root_sub, sizeof root_sub, "%s/sysdata", env_root);
        prefixes[np++] = root_sub;
        prefixes[np++] = env_root;
    }
    prefixes[np++] = "Contents/Resources/dm/sysdata";
    prefixes[np++] = "dm/sysdata";
    prefixes[np] = NULL;
    for (int p = 0; prefixes[p]; p++) {
        snprintf(path, sizeof path, "%s/sysdata_%016llx.romfs", prefixes[p], (unsigned long long)tid);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size > 0) {
                uint8_t *mapped = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
                if (mapped != MAP_FAILED) {
                    LOG("sf: loaded sysdata RomFS for %016llx from %s (size %zu KB)",
                        (unsigned long long)tid, path, (size_t)st.st_size / 1024);
                    if (g_num_sysdata < 16) {
                        int idx = g_num_sysdata++;
                        g_sysdata_cache[idx].tid = tid;
                        g_sysdata_cache[idx].ctx.fd = fd;
                        g_sysdata_cache[idx].ctx.size = (size_t)st.st_size;
                        g_sysdata_cache[idx].ctx.mapped = mapped;
                        return &g_sysdata_cache[idx].ctx;
                    }
                }
            }
            close(fd);
        }
    }
    LOG("[YELLOW] sf: sysdata RomFS not found for %016llx, falling back to base RomFS", (unsigned long long)tid);
    return get_rom_storage();
}

static fsp_storage_ctx *get_rom_storage(void) {
    if (g_rom_storage) return g_rom_storage;
    char root_buf[1024], root_res[1024], root_plug[1024];
    const char *paths[10] = { 0 };
    int npaths = 0;
    const char *env_bin = getenv("DM_ROMFS_BIN");
    if (env_bin && *env_bin) paths[npaths++] = env_bin;
    const char *env_rom = getenv("DM_ROMFS");
    if (env_rom && *env_rom) paths[npaths++] = env_rom;
    const char *res = getenv("DM_ROOT");
    if (res && *res) {
        snprintf(root_buf, sizeof root_buf, "%s/romfs.bin", res);
        paths[npaths++] = root_buf;
        snprintf(root_res, sizeof root_res, "%s/../romfs.bin", res);
        paths[npaths++] = root_res;
        snprintf(root_plug, sizeof root_plug, "%s/../PlugIns/GameData.bundle/Contents/Resources/romfs.img", res);
        paths[npaths++] = root_plug;
    }
    paths[npaths++] = "Contents/Resources/dm/romfs.bin";
    paths[npaths++] = "Contents/Resources/romfs.bin";
    paths[npaths++] = "Contents/PlugIns/GameData.bundle/Contents/Resources/romfs.img";

    for (int i = 0; i < npaths; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) {
            fsp_storage_ctx *ctx = calloc(1, sizeof *ctx);
            ctx->fd = fd;
            ctx->size = (size_t)st.st_size;
            ctx->mapped = mmap(NULL, ctx->size, PROT_READ, MAP_SHARED, fd, 0);
            if (ctx->mapped != MAP_FAILED) {
                LOG("sf: loaded RomFS binary from %s (size %zu MB)", paths[i], ctx->size / (1024*1024));
                g_rom_storage = ctx;
                return g_rom_storage;
            }
            free(ctx);
            close(fd);
        }
    }
    LOG("[RED] sf: could not find or open romfs.bin");
    return NULL;
}

static Result fsp_storage_answer(sf_req *q) {
    fsp_storage_ctx *ctx = (fsp_storage_ctx *)q->ctx;
    uint64_t off = q->in_len >= 8 ? *(uint64_t *)q->in : 0;
    uint64_t sz = q->in_len >= 16 ? *(uint64_t *)(q->in + 8) : 0;
    LOG("sf: IStorage cmd=%u off=%#llx sz=%#llx in_len=%zu nb=%u b0_sz=%llu b0_p=%p ctx_sz=%#zx",
        q->cmd, (unsigned long long)off, (unsigned long long)sz, q->in_len,
        q->nb, q->nb > 0 ? (unsigned long long)q->b[0].size : 0ULL,
        q->nb > 0 ? q->b[0].p : NULL, ctx ? ctx->size : 0);
    if (q->cmd == 0) { // Read(u64 offset, u64 size)
        if (ctx && q->nb > 0 && q->b[0].p) {
            size_t copy_sz = q->b[0].size;
            if (copy_sz > sz) copy_sz = sz;
            if (off >= ctx->size) copy_sz = 0;
            else if (off + copy_sz > ctx->size) copy_sz = ctx->size - off;
            if (copy_sz && ctx->mapped) memcpy(q->b[0].p, ctx->mapped + off, copy_sz);
        }
        return RES_OK;
    }
    if (q->cmd == 4) { // GetSize -> u64
        q->out_len = 8;
        *(uint64_t *)q->out = ctx ? ctx->size : 0;
        return RES_OK;
    }
    return RES_OK;
}

typedef struct {
    int fd;
    char path[1024];
} fsp_file_ctx;

static void fsp_file_close(void *ctx) {
    fsp_file_ctx *c = (fsp_file_ctx *)ctx;
    if (c) {
        if (c->fd >= 0) close(c->fd);
        free(c);
    }
}

static Result fsp_file_answer(sf_req *q) {
    fsp_file_ctx *ctx = (fsp_file_ctx *)q->ctx;
    if (!ctx || ctx->fd < 0) return 0x202; // PathNotFound
    if (q->cmd == 0) { // Read(u32 opt, u64 off, u64 sz)
        uint64_t off = q->in_len >= 16 ? *(uint64_t *)(q->in + 8) : 0;
        uint64_t sz = q->in_len >= 24 ? *(uint64_t *)(q->in + 16) : 0;
        if (q->nb > 0 && q->b[0].p && q->b[0].size > 0) {
            size_t to_read = sz < q->b[0].size ? sz : q->b[0].size;
            ssize_t rd = pread(ctx->fd, q->b[0].p, to_read, (off_t)off);
            if (rd < 0) rd = 0;
            q->out_len = 8;
            *(uint64_t *)q->out = (uint64_t)rd;
            return RES_OK;
        }
        return RES_OK;
    }
    if (q->cmd == 1) { // Write(u32 opt, u64 off, u64 sz)
        uint64_t off = q->in_len >= 16 ? *(uint64_t *)(q->in + 8) : 0;
        uint64_t sz = q->in_len >= 24 ? *(uint64_t *)(q->in + 16) : 0;
        if (q->na > 0 && q->a[0].p && q->a[0].size > 0) {
            size_t to_write = sz < q->a[0].size ? sz : q->a[0].size;
            pwrite(ctx->fd, q->a[0].p, to_write, (off_t)off);
            return RES_OK;
        }
        return RES_OK;
    }
    if (q->cmd == 2) { // Flush
        fsync(ctx->fd);
        return RES_OK;
    }
    if (q->cmd == 3) { // SetSize(u64 sz)
        uint64_t sz = q->in_len >= 8 ? *(uint64_t *)q->in : 0;
        ftruncate(ctx->fd, (off_t)sz);
        return RES_OK;
    }
    if (q->cmd == 4) { // GetSize -> u64
        struct stat st;
        fstat(ctx->fd, &st);
        q->out_len = 8;
        *(uint64_t *)q->out = (uint64_t)st.st_size;
        return RES_OK;
    }
    if (q->cmd == 5) { // OperateRange
        return RES_OK;
    }
    LOG("sf: IFile unknown cmd %u", q->cmd);
    return RES_OK;
}

static const char *get_save_dir(void) {
    static char buf[1024];
    const char *d = getenv("DM_SAVE");
    if (d && *d) return d;
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, sizeof buf, "%s/Library/Application Support/nxnative/0100000000001009/save", home);
        mkdir(buf, 0755);
        return buf;
    }
    snprintf(buf, sizeof buf, "save");
    mkdir(buf, 0755);
    return buf;
}

static Result fsp_filesystem_answer(sf_req *q) {
    LOG("sf: IFileSystem cmd=%u in_len=%zu na=%u nx=%u", q->cmd, q->in_len, q->na, q->nx);
    const char *save_dir = get_save_dir();
    char subpath[512] = {0};
    const char *raw_path = NULL;
    if (q->nx > 0 && q->x[0].p && q->x[0].size > 0) raw_path = (const char *)q->x[0].p;
    else if (q->na > 0 && q->a[0].p && q->a[0].size > 0) raw_path = (const char *)q->a[0].p;

    if (raw_path) {
        const char *p = raw_path;
        if (strncmp(p, "save:/", 6) == 0) p += 6;
        while (*p == ':' || *p == '/') p++;
        strlcpy(subpath, p, sizeof subpath);
    }

    char full_path[1024];
    snprintf(full_path, sizeof full_path, "%s/%s", save_dir, subpath);

    if (q->cmd == 0) { // CreateFile(mode, size, ...)
        uint64_t size = q->in_len >= 16 ? *(uint64_t *)(q->in + 8) : 0;
        int fd = open(full_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (size > 0) ftruncate(fd, (off_t)size);
            close(fd);
            LOG("sf: IFileSystem CreateFile('%s', size=%llu) succeeded", full_path, (unsigned long long)size);
            return RES_OK;
        }
        LOG("[RED] sf: IFileSystem CreateFile('%s') failed: errno=%d", full_path, errno);
        return 0x202; // PathNotFound
    }
    if (q->cmd == 1) { // DeleteFile
        unlink(full_path);
        return RES_OK;
    }
    if (q->cmd == 8) { // OpenFile(mode)
        uint32_t mode = q->in_len >= 4 ? *(uint32_t *)q->in : 3;
        int flags = O_RDONLY;
        if ((mode & 3) == 3) flags = O_RDWR;
        else if (mode & 2) flags = O_WRONLY;
        int fd = open(full_path, flags);
        if (fd < 0) {
            LOG("sf: IFileSystem OpenFile('%s', mode=%u) -> not found (0x202)", full_path, mode);
            return 0x202; // ResultPathNotFound
        }
        fsp_file_ctx *fctx = calloc(1, sizeof *fctx);
        fctx->fd = fd;
        strlcpy(fctx->path, full_path, sizeof fctx->path);
        q->out_move = sf_session("IFile", fsp_file_answer, fctx, fsp_file_close);
        LOG("sf: IFileSystem OpenFile('%s', mode=%u) -> IFile", full_path, mode);
        return RES_OK;
    }
    if (q->cmd == 10) { // Commit
        return RES_OK;
    }
    if (q->cmd == 11) { // GetFreeSpaceSize
        q->out_len = 8;
        *(uint64_t *)q->out = 1ULL * 1024 * 1024 * 1024; // 1 GB
        return RES_OK;
    }
    if (q->cmd == 12) { // GetTotalSpaceSize
        q->out_len = 8;
        *(uint64_t *)q->out = 10ULL * 1024 * 1024 * 1024; // 10 GB
        return RES_OK;
    }
    return RES_OK;
}

static Result fsp_srv_answer(sf_req *q) {
    if (q->cmd == 1) { // SetCurrentProcess
        return RES_OK;
    }
    if (q->cmd == 23 || q->cmd == 24) { // CreateSaveDataFileSystem
        return RES_OK;
    }
    if (q->cmd == 51 || q->cmd == 52) { // OpenSaveDataFileSystem / OpenSaveDataFileSystemBySystemSaveDataId
        q->out_move = sf_session("IFileSystem", fsp_filesystem_answer, NULL, NULL);
        LOG("sf: fsp-srv command %u -> IFileSystem (SaveData)", q->cmd);
        return RES_OK;
    }
    if (q->cmd == 202) { // OpenDataStorageByProgramId
        uint64_t tid = q->in_len >= 16 ? *(uint64_t *)(q->in + 8) : 0;
        fsp_storage_ctx *ctx = (tid && tid != 0x0100000000001009ULL) ? get_sysdata_storage(tid) : get_rom_storage();
        q->out_move = sf_session("IStorage", fsp_storage_answer, ctx, NULL);
        LOG("sf: fsp-srv command 202 OpenDataStorageByProgramId(%016llx) -> IStorage", (unsigned long long)tid);
        return RES_OK;
    }
    if (q->cmd == 200 || q->cmd == 18 || q->cmd == 201 || q->cmd == 203 || q->cmd == 204 || q->cmd == 205) {
        // OpenDataStorageByCurrentProcess / OpenRomStorage
        fsp_storage_ctx *ctx = get_rom_storage();
        q->out_move = sf_session("IStorage", fsp_storage_answer, ctx, NULL);
        LOG("sf: fsp-srv command %u -> IStorage (RomFS)", q->cmd);
        return RES_OK;
    }
    LOG("sf: fsp-srv command %u (in_len=%zu)", q->cmd, q->in_len);
    return RES_OK;
}
SF_SERVICE("fsp-srv", fsp_srv_answer);

// HID shared memory of a library applet that links nn::hid statically (MiiEdit): the SDK inside it reads the console's hid shared memory directly, so the host fills it, in the real layout, from the
// same input the front end publishes for a game (keyboard, controllers, mouse as touch; dm_input_peek / touch_fill). Layout (libnx HidSharedMemory, confirmed against MiiEdit's accessor at
// main+0x2a5318, which points at ten 0x5000-byte npad entries from +0x9a00, its npad lifo reader at main+0x2a4894 reading +0x1408 with stride 0x30, and its touch lifo reader at main+0x2a5b30 reading
// +0x400 with stride 0x298): touch screen lifo at +0x400; npad entry i at +0x9a00 + i * 0x5000, i = 0..7 No1..No8, 8 Handheld, 9 Other; in an entry +0 style set, +4 joy assignment mode, then seven
// lifos from +0x28 (full key, handheld, joy dual, joy left, joy right, palma, system ext at +0x1408), 0x350 apart, each a 0x20-byte header {unused, buffer count, tail, count} and 17 AtomicStorage<T>
// slots {u64 atomic_sampling_number, T payload}
#include "dm_input.h"
#include <time.h>

static dm_applet_shm_t *applet_shm_map(void) {
    static dm_applet_shm_t *s_shm = NULL;
    static int s_init = 0;
    if (!s_init) {
        s_init = 1;
        const char *p = getenv("DM_APPLET_SHM");
        if (p && *p) {
            int fd = open(p, O_RDWR);
            if (fd >= 0) {
                void *m = mmap(NULL, sizeof(dm_applet_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (m != MAP_FAILED) s_shm = (dm_applet_shm_t *)m;
                close(fd);
            }
        }
    }
    return s_shm;
}
int touch_fill(void *out, uint64_t now_ns);   // hle_touch.c: one TouchScreenState<16>, 0x290 bytes, the payload of a touch lifo slot
enum { HID_TOUCH = 0x400, HID_NPAD = 0x9a00, HID_NPAD_STRIDE = 0x5000, HID_NPAD_COUNT = 10, HID_NPAD_HANDHELD = 8,
       NPAD_LIFO0 = 0x28, NPAD_LIFO_SIZE = 0x350, NPAD_LIFOS = 7, LIFO_ENTRIES = 17, NPAD_ENTRY = 0x28, TOUCH_ENTRY = 0x290 };
static uint8_t g_hid_shmem[0x40000] __attribute__((aligned(16384)));
static uint32_t g_hid_shmem_h = 0;

static void hid_lifo_init(uint8_t *lifo) { uint64_t h[4] = {0, LIFO_ENTRIES, LIFO_ENTRIES - 1, 0}; memcpy(lifo, h, sizeof h); }
// Append one AtomicStorage<T> slot (8-byte atomic_sampling_number prefix followed by the esz-byte payload, whose first 8 bytes are also sampling_number): the SDK's lifo reader checks that the prefix
// matches the payload's sampling_number and that consecutive slots differ by 1
static void hid_lifo_push(uint8_t *lifo, size_t esz, const void *entry) {
    size_t stride = 8 + esz;
    uint64_t *h = (uint64_t *)lifo, tail = (h[2] + 1) % LIFO_ENTRIES, count = h[3] < LIFO_ENTRIES ? h[3] + 1 : LIFO_ENTRIES;
    uint8_t *slot = lifo + 0x20 + tail * stride;
    uint64_t samp = *(const uint64_t *)entry;
    memcpy(slot + 8, entry, esz);
    __atomic_store_n((uint64_t *)slot, samp, __ATOMIC_RELEASE);
    __atomic_store_n(&h[3], count, __ATOMIC_RELEASE);
    __atomic_store_n(&h[2], tail, __ATOMIC_RELEASE);
}
static uint64_t hid_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void *hid_feed_main(void *arg) {
    (void)arg;
    enum { STYLE_FULLKEY = 1, STYLE_HANDHELD = 2, STYLE_JOYDUAL = 4, STYLE_JOYLEFT = 8, STYLE_JOYRIGHT = 16, STICK = 0x4000 };
    uint8_t *touch = g_hid_shmem + HID_TOUCH;
    hid_lifo_init(touch);
    for (int i = 0; i < HID_NPAD_COUNT; i++) for (int s = 0; s < NPAD_LIFOS; s++) hid_lifo_init(g_hid_shmem + HID_NPAD + (size_t)i * HID_NPAD_STRIDE + NPAD_LIFO0 + (size_t)s * NPAD_LIFO_SIZE);
    int64_t sampling = 0;
    dm_applet_shm_t *shm = applet_shm_map();
    for (;;) {
        uint64_t b = 0; int32_t a[4] = {0};
        if (shm) {
            uint64_t kb = atomic_load_explicit(&shm->kbd_btn, memory_order_relaxed);
            uint64_t pb = atomic_load_explicit(&shm->pad_btn, memory_order_relaxed);
            b = kb | pb;
            int32_t ka[4] = {
                atomic_load_explicit(&shm->kbd_lx, memory_order_relaxed),
                atomic_load_explicit(&shm->kbd_ly, memory_order_relaxed),
                atomic_load_explicit(&shm->kbd_rx, memory_order_relaxed),
                atomic_load_explicit(&shm->kbd_ry, memory_order_relaxed),
            };
            int32_t pa[4] = {
                atomic_load_explicit(&shm->pad_lx, memory_order_relaxed),
                atomic_load_explicit(&shm->pad_ly, memory_order_relaxed),
                atomic_load_explicit(&shm->pad_rx, memory_order_relaxed),
                atomic_load_explicit(&shm->pad_ry, memory_order_relaxed),
            };
            for (int k = 0; k < 4; k++) a[k] = abs(pa[k]) > abs(ka[k]) ? pa[k] : ka[k];
            int32_t tn = atomic_load_explicit(&shm->touch_n, memory_order_acquire);
            if (tn < 0) tn = 0; if (tn > 16) tn = 16;
            dm_touch_set(tn ? shm->touch : NULL, tn);
        } else {
            dm_input_peek(&b, a);
        }
        // The stick directions as buttons (NpadButton 16..23), as the host derives them for a game
        if (a[0] < -STICK) b |= 1u << 16; if (a[1] > STICK) b |= 1u << 17; if (a[0] > STICK) b |= 1u << 18; if (a[1] < -STICK) b |= 1u << 19;
        if (a[2] < -STICK) b |= 1u << 20; if (a[3] > STICK) b |= 1u << 21; if (a[2] > STICK) b |= 1u << 22; if (a[3] < -STICK) b |= 1u << 23;
        sampling++;
        for (int i = 0; i < HID_NPAD_COUNT; i++) {
            uint8_t *e = g_hid_shmem + HID_NPAD + (size_t)i * HID_NPAD_STRIDE;
            if (i != 0 && i != HID_NPAD_HANDHELD) continue;   // player 1, as the 1P controller and as the handheld one: the editor takes whichever it supports
            uint32_t styles = STYLE_FULLKEY | STYLE_HANDHELD | STYLE_JOYDUAL; memcpy(e, &styles, 4);
            uint32_t dev_type = (i == HID_NPAD_HANDHELD) ? 2u : 1u; memcpy(e + 0x4188, &dev_type, 4);
            uint64_t sys_prop = 0x800u; memcpy(e + 0x4190, &sys_prop, 8);   // is_abxy_button_oriented
            uint32_t footer = 1u; memcpy(e + 0x41a8, &footer, 4);
            e[0x41ac] = 4;                                                  // full battery
            for (int s = 0; s < NPAD_LIFOS; s++) {
                if (s == 3 || s == 4 || s == 5) continue;   // Joy-Con left / right alone, Palma: not connected
                uint32_t attr = s == 0 ? 0x3u : s == 1 ? 0x3fu : 0x15u;   // connected (+ wired; each Joy-Con side of a pair or the handheld)
                uint8_t ent[NPAD_ENTRY]; memset(ent, 0, sizeof ent);
                memcpy(ent, &sampling, 8); memcpy(ent + 8, &b, 8); memcpy(ent + 16, a, 16); memcpy(ent + 32, &attr, 4);
                hid_lifo_push(e + NPAD_LIFO0 + (size_t)s * NPAD_LIFO_SIZE, NPAD_ENTRY, ent);
            }
        }
        uint8_t te[TOUCH_ENTRY]; touch_fill(te, hid_now_ns());
        hid_lifo_push(touch, TOUCH_ENTRY, te);
        usleep(16000);   // ~60 Hz, matching the applet's frame rate so short touch latches are not consumed between frames
    }
    return NULL;
}

static uint32_t get_hid_shmem_handle(void) {
    if (!g_hid_shmem_h) {
        g_hid_shmem_h = hle_new_shmem(g_hid_shmem, sizeof g_hid_shmem);
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, hid_feed_main, NULL)) LOG("[RED] sf: hid: the shared memory feeder did not start: the applet gets no input");
        else LOG("sf: hid: shared memory fed from the host's input (npad 1 and handheld, touch screen)");
    }
    return g_hid_shmem_h;
}

static Result applet_resource_answer(sf_req *q) {
    if (q->cmd == 0) { // GetSharedMemoryHandle
        q->out_copy = get_hid_shmem_handle();
        LOG("sf: IAppletResource GetSharedMemoryHandle -> %#x", q->out_copy);
        return RES_OK;
    }
    return RES_OK;
}

static Result hid_answer(sf_req *q) {
    LOG("sf: hid command %u (%zu data words in)", q->cmd, q->in_len / 4);
    if (q->cmd == 0) { // CreateAppletResource
        q->out_move = sf_session("IAppletResource", applet_resource_answer, NULL, NULL);
        LOG("sf: hid CreateAppletResource -> IAppletResource");
        return RES_OK;
    }
    if (q->cmd == 101) { // GetSupportedNpadStyleSet
        q->out_len = 4;
        *(uint32_t *)q->out = 0x1f; // FullKey | Handheld | JoyDual | JoyLeft | JoyRight
        return RES_OK;
    }
    if (q->cmd == 106) { // AcquireNpadStyleSetUpdateEventHandle
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    if (q->cmd == 108) { // GetPlayerLedPattern
        q->out_len = 8;
        *(uint64_t *)q->out = 1;
        return RES_OK;
    }
    if (q->cmd == 121) { // GetNpadJoyHoldType
        q->out_len = 8;
        *(uint64_t *)q->out = 0; // Vertical
        return RES_OK;
    }
    return RES_OK;
}
SF_SERVICE("hid", hid_answer);

static Result hid_sys_answer(sf_req *q) {
    LOG("sf: hid:sys command %u (%zu data words in)", q->cmd, q->in_len / 4);
    if (q->cmd == 101 || q->cmd == 121 || q->cmd == 141) {
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    return RES_OK;
}
SF_SERVICE("hid:sys", hid_sys_answer);

static uint32_t g_audout_state = 1; // 0 = Started, 1 = Stopped
static struct { uint64_t tag, ready_ns; } g_audout_tags[32];
static int g_audout_ntags = 0;
static pthread_mutex_t g_audout_mu = PTHREAD_MUTEX_INITIALIZER;

static Result audio_out_answer(sf_req *q) {
    if (q->cmd == 0) { // GetAudioOutState
        q->out_len = 4;
        *(uint32_t *)q->out = g_audout_state;
        return RES_OK;
    }
    if (q->cmd == 1) { // StartAudioOut
        g_audout_state = 0;
        return RES_OK;
    }
    if (q->cmd == 2) { // StopAudioOut
        g_audout_state = 1;
        return RES_OK;
    }
    if (q->cmd == 3 || q->cmd == 7) { // AppendAudioOutBuffer / AppendAudioOutBufferAuto
        uint64_t tag = q->in_len >= 8 ? *(uint64_t *)q->in : 0;
        uint64_t ready = hid_now_ns() + 4000000ull; // 4 ms hardware playback duration
        pthread_mutex_lock(&g_audout_mu);
        if (tag && g_audout_ntags < 32) g_audout_tags[g_audout_ntags++] = (typeof(g_audout_tags[0])){tag, ready};
        pthread_mutex_unlock(&g_audout_mu);
        return RES_OK;
    }
    if (q->cmd == 4 || q->cmd == 9) { // RegisterBufferEvent: unsignaled so SoundThread paces at its 5 ms WaitSynchronization timeout
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    if (q->cmd == 5 || q->cmd == 8) { // GetReleasedAudioOutBuffer / GetReleasedAudioOutBuffersAuto
        uint32_t released = 0;
        uint64_t now = hid_now_ns();
        pthread_mutex_lock(&g_audout_mu);
        if (q->nb > 0 && q->b[0].p && q->b[0].size >= 8) {
            memset(q->b[0].p, 0, (size_t)q->b[0].size);
            size_t cap = q->b[0].size / 8;
            uint64_t *dst = (uint64_t *)q->b[0].p;
            while (released < cap && g_audout_ntags > 0 && g_audout_tags[0].ready_ns <= now) {
                dst[released++] = g_audout_tags[0].tag;
                memmove(&g_audout_tags[0], &g_audout_tags[1], (size_t)(--g_audout_ntags) * sizeof(g_audout_tags[0]));
            }
        }
        pthread_mutex_unlock(&g_audout_mu);
        q->out_len = 4;
        *(uint32_t *)q->out = released;
        return RES_OK;
    }
    if (q->cmd == 6) { // ContainsAudioOutBuffer
        q->out_len = 1;
        q->out[0] = 0;
        return RES_OK;
    }
    if (q->cmd == 10) { // GetAudioOutBufferCount
        pthread_mutex_lock(&g_audout_mu);
        uint32_t cnt = (uint32_t)g_audout_ntags;
        pthread_mutex_unlock(&g_audout_mu);
        q->out_len = 4;
        *(uint32_t *)q->out = cnt;
        return RES_OK;
    }
    if (q->cmd == 11) { // GetAudioOutVolume
        q->out_len = 4;
        *(float *)q->out = 1.0f;
        return RES_OK;
    }
    return RES_OK;
}

static Result audout_answer(sf_req *q) {
    if (q->cmd == 0 || q->cmd == 2) { // ListAudioOuts, ListAudioOutsAuto
        q->out_len = 4;
        *(uint32_t *)q->out = 1;
        return RES_OK;
    }
    if (q->cmd == 1 || q->cmd == 3) { // OpenAudioOut, OpenAudioOutAuto
        q->out_move = sf_session("IAudioOut", audio_out_answer, NULL, NULL);
        q->out_copy = hle_new_event(NULL);
        q->out_len = 16;
        uint32_t *out = (uint32_t *)q->out;
        out[0] = 48000;
        out[1] = 2; // 2 channels
        out[2] = 2; // INT16
        out[3] = 1; // Stopped
        return RES_OK;
    }
    return RES_OK;
}
SF_SERVICE("audout:u", audout_answer);

static Result manager_display_service_answer(sf_req *q) {
    LOG("sf: IManagerDisplayService command %u (%zu data words in)", q->cmd, q->in_len / 4);
    if (q->cmd == 2012 || q->cmd == 2030 || q->cmd == 2312) { // CreateStrayLayer
        q->out_len = 16;
        uint64_t *out = (uint64_t *)q->out;
        out[0] = 1; // Layer ID
        out[1] = 0x4e49574e; // Native window handle (NWNI)
        if (q->nb > 0 && q->b[0].p && q->b[0].size > 0) {
            memset(q->b[0].p, 0, (size_t)q->b[0].size);
            if (q->b[0].size >= 8) {
                *(uint32_t *)q->b[0].p = 0x544e4742; // BGN (buffer producer parcel)
            }
        }
        return RES_OK;
    }
    if (q->cmd == 2013 || q->cmd == 2031) { // DestroyStrayLayer
        return RES_OK;
    }
    if (q->cmd == 2050 || q->cmd == 2060) { // AddToLayerStack, SetLayerVisibility
        return RES_OK;
    }
    return RES_OK;
}

static Result app_display_service_answer(sf_req *q) {
    LOG("sf: IApplicationDisplayService command %u (%zu data words in)", q->cmd, q->in_len / 4);
    if (q->cmd == 100) { // GetRelayService -> IHOSBinderDriver
        q->out_move = sf_session("IHOSBinderDriver", manager_display_service_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 101) { // GetSystemDisplayService -> ISystemDisplayService
        q->out_move = sf_session("ISystemDisplayService", manager_display_service_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 102) { // GetManagerDisplayService -> IManagerDisplayService
        q->out_move = sf_session("IManagerDisplayService", manager_display_service_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 103) { // GetIndirectDisplayTransactionService -> IHOSBinderDriver
        q->out_move = sf_session("IHOSBinderDriver", manager_display_service_answer, NULL, NULL);
        return RES_OK;
    }
    if (q->cmd == 1010 || q->cmd == 1011) { // OpenDisplay, OpenDefaultDisplay
        q->out_len = 8;
        *(uint64_t *)q->out = 1; // Display ID 1
        return RES_OK;
    }
    if (q->cmd == 1020) { // CloseDisplay
        return RES_OK;
    }
    if (q->cmd == 1102) { // GetDisplayResolution
        q->out_len = 16;
        uint64_t *out = (uint64_t *)q->out;
        out[0] = 1280;
        out[1] = 720;
        return RES_OK;
    }
    if (q->cmd == 2020 || q->cmd == 2030 || q->cmd == 2012 || q->cmd == 2312) { // OpenLayer / CreateStrayLayer
        q->out_len = 16;
        uint64_t *out = (uint64_t *)q->out;
        out[0] = 0x3c; // Layer ID / Parcel size
        out[1] = 0x3c; // Parcel size (0x10 header + 0x28 flat_binder_object + 0x4 object offset)
        if (q->nb > 0 && q->b[0].p && q->b[0].size >= 0x3c) {
            memset(q->b[0].p, 0, (size_t)q->b[0].size);
            uint32_t *p32 = (uint32_t *)q->b[0].p;
            p32[0] = 0x28; // data_size
            p32[1] = 0x10; // data_offset
            p32[2] = 0x04; // objects_size
            p32[3] = 0x38; // objects_offset
            p32[4] = 2;    // flat_binder_object: type = 2 (BINDER_TYPE_BINDER)
            p32[5] = 0;    // flags
            p32[6] = 1;    // binder_id = 1
            p32[7] = 0;
            p32[8] = 1;    // cookie = 1
            p32[9] = 0;
            memcpy(&p32[10], "dispdrv", 8);
            p32[14] = 0;   // objects[0] = offset 0 in data
        } else if (q->nb > 0 && q->b[0].p && q->b[0].size > 0) {
            memset(q->b[0].p, 0, (size_t)q->b[0].size);
        }
        return RES_OK;
    }
    if (q->cmd == 2021 || q->cmd == 2031) { // CloseLayer / DestroyStrayLayer
        return RES_OK;
    }
    if (q->cmd == 2101) { // SetLayerScalingMode
        return RES_OK;
    }
    if (q->cmd == 5202) { // GetDisplayVsyncEvent
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    }
    return RES_OK;
}

static Result vi_root_answer(sf_req *q) {
    LOG("sf: vi root command %u (%zu data words in)", q->cmd, q->in_len / 4);
    if (q->cmd == 2) { // GetDisplayService(u32 type) -> IApplicationDisplayService
        q->out_move = sf_session("IApplicationDisplayService", app_display_service_answer, NULL, NULL);
        return RES_OK;
    }
    return RES_OK;
}
static Result vi_m_answer(sf_req *q) { return vi_root_answer(q); }
static Result vi_u_answer(sf_req *q) { return vi_root_answer(q); }
static Result vi_s_answer(sf_req *q) { return vi_root_answer(q); }
SF_SERVICE("vi:m", vi_m_answer);
SF_SERVICE("vi:u", vi_u_answer);
SF_SERVICE("vi:s", vi_s_answer);

static Result set_root_answer(sf_req *q) {
    LOG("sf: set command %u (%zu data words in, nb=%u)", q->cmd, q->in_len / 4, q->nb);
    if (q->cmd == 0) { // GetLanguageCode
        const char *e = getenv("DM_LANG"); if (!e || !*e) e = "en-US";
        char code[8] = {0}; strncpy(code, e, sizeof code - 1);
        q->out_len = 8;
        memcpy(q->out, code, 8);
        return RES_OK;
    }
    if (q->cmd == 1 || q->cmd == 5) { // GetAvailableLanguageCodes / 2
        if (q->nb > 0 && q->b[0].p && q->b[0].size >= 8) {
            const char *e = getenv("DM_LANG"); if (!e || !*e) e = "en-US";
            char code[8] = {0}; strncpy(code, e, sizeof code - 1);
            memcpy(q->b[0].p, code, 8);
            q->out_len = 4;
            *(int32_t *)q->out = 1;
        }
        return RES_OK;
    }
    if (q->cmd == 3 || q->cmd == 6) { // GetAvailableLanguageCodeCount / 2
        q->out_len = 4;
        *(int32_t *)q->out = 1;
        return RES_OK;
    }
    if (q->cmd == 4) { // GetRegionCode
        const char *e = getenv("DM_LANG"); if (!e || !*e) e = "en-US";
        int r = !strncmp(e, "ja", 2) ? 0 : !strncmp(e, "ko", 2) || !strncmp(e, "zh", 2) ? 4 : !strcmp(e, "en-US") || !strcmp(e, "fr-CA") || !strcmp(e, "es-419") || !strcmp(e, "pt-BR") ? 1 : 2;
        q->out_len = 4;
        *(int32_t *)q->out = r;
        return RES_OK;
    }
    if (q->cmd == 8) { // GetQuestFlag
        q->out_len = 1;
        *(uint8_t *)q->out = 0;
        return RES_OK;
    }
    if (q->cmd == 11) { // GetDeviceNickName
        if (q->nb > 0 && q->b[0].p && q->b[0].size > 0) {
            memset(q->b[0].p, 0, (size_t)q->b[0].size);
            strncpy((char *)q->b[0].p, "Switch", (size_t)q->b[0].size - 1);
        }
        return RES_OK;
    }
    if (q->cmd == 90) { // GetMiiAuthorId -> nn::util::Uuid (16 bytes)
        q->out_len = 16;
        memset(q->out, 0, 16);
        return RES_OK;
    }
    return RES_OK;
}
static Result set_answer(sf_req *q) { return set_root_answer(q); }
static Result set_sys_answer(sf_req *q) { return set_root_answer(q); }
static Result set_cal_answer(sf_req *q) { return set_root_answer(q); }
static Result set_fd_answer(sf_req *q) { return set_root_answer(q); }
SF_SERVICE("set", set_answer);
SF_SERVICE("set:sys", set_sys_answer);
SF_SERVICE("set:cal", set_cal_answer);
SF_SERVICE("set:fd", set_fd_answer);

// ---------- nfc:sys / nfc:user (ISystemManager / ISystem) ----------
static Result nfc_system_answer(sf_req *q) {
    LOG("nfc:system cmd=%u in_len=%u", q->cmd, (unsigned)q->in_len);
    switch (q->cmd) {
    case 0:   // InitializeNfc
    case 400: // Initialize
        return RES_OK;
    case 1:   // FinalizeNfc
    case 401: // Finalize
        return RES_OK;
    case 2:   // GetStateNfc
    case 402: // GetState
        q->out_len = 4;
        *(uint32_t *)q->out = 1; // 1 = Initialized
        return RES_OK;
    case 3:   // IsNfcEnabledNfc
    case 403: // IsNfcEnabled
        q->out_len = 1;
        q->out[0] = 1;
        return RES_OK;
    case 404: // ListDevices
        q->out_len = 4;
        *(uint32_t *)q->out = 0; // 0 devices
        return RES_OK;
    case 405: // GetDeviceState
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    case 406: // GetNpadId
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    case 407: // GetAvailabilityChangeEventHandle
    case 411: // GetActivateEventHandle
    case 412: // GetDeactivateEventHandle
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    case 408: // StartDetection
    case 409: // StopDetection
    case 500: // SetNfcEnabled
        return RES_OK;
    default:
        LOG("nfc:system unhandled cmd=%u", q->cmd);
        return RES_OK;
    }
}

static Result nfc_sys_answer(sf_req *q) {
    LOG("nfc:sys cmd=%u in_len=%u", q->cmd, (unsigned)q->in_len);
    if (q->cmd == 0) { // CreateSystemInterface -> returns ISystem
        q->out_move = sf_session("ISystem", nfc_system_answer, NULL, NULL);
        return RES_OK;
    }
    return nfc_system_answer(q);
}

static Result nfc_user_answer(sf_req *q) {
    return nfc_sys_answer(q);
}
SF_SERVICE("nfc:sys", nfc_sys_answer);
SF_SERVICE("nfc:user", nfc_user_answer);

// ---------- nfp:sys / nfp:user (ISystemManager / IUserManager / INfp) ----------
static Result nfp_interface_answer(sf_req *q) {
    LOG("nfp:interface cmd=%u in_len=%u", q->cmd, (unsigned)q->in_len);
    switch (q->cmd) {
    case 0:   // Initialize
    case 1:   // Finalize
        return RES_OK;
    case 2:   // ListDevices -> returns u32 count = 0
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    case 3:   // StartDetection
    case 4:   // StopDetection
        return RES_OK;
    case 5:   // Mount
    case 6:   // Unmount
        return (115u | (97u << 9)); // TagNotFound
    case 17:  // AttachActivateEvent
    case 18:  // AttachDeactivateEvent
    case 23:  // AttachAvailabilityChangeEvent
        q->out_copy = hle_new_event(NULL);
        return RES_OK;
    case 19:  // GetState
        q->out_len = 4;
        *(uint32_t *)q->out = 1; // Initialized
        return RES_OK;
    case 20:  // GetDeviceState
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    case 21:  // GetNpadId
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    case 22:  // GetApplicationAreaSize
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    default:
        LOG("nfp:interface unhandled cmd=%u", q->cmd);
        return RES_OK;
    }
}

static Result nfp_sys_answer(sf_req *q) {
    LOG("nfp:sys cmd=%u in_len=%u", q->cmd, (unsigned)q->in_len);
    if (q->cmd == 0) { // CreateSystemInterface -> returns INfp
        q->out_move = sf_session("INfp", nfp_interface_answer, NULL, NULL);
        return RES_OK;
    }
    return nfp_interface_answer(q);
}

static Result nfp_user_answer(sf_req *q) {
    LOG("nfp:user cmd=%u in_len=%u", q->cmd, (unsigned)q->in_len);
    if (q->cmd == 0) { // CreateUserInterface -> returns INfp
        q->out_move = sf_session("INfp", nfp_interface_answer, NULL, NULL);
        return RES_OK;
    }
    return nfp_interface_answer(q);
}
SF_SERVICE("nfp:sys", nfp_sys_answer);
SF_SERVICE("nfp:user", nfp_user_answer);

static uint32_t g_next_nv_fd = 1;
static Result nvdrv_root_answer(sf_req *q) {
    if (q->cmd == 0) { // Open -> (u32 fd, u32 error_code)
        q->out_len = 8;
        uint32_t *out = (uint32_t *)q->out;
        out[0] = g_next_nv_fd++;
        out[1] = 0;
        return RES_OK;
    }
    if (q->cmd == 1 || q->cmd == 11 || q->cmd == 12) { // Ioctl / Ioctl2 / Ioctl3 -> u32 error_code = 0
        if (q->nb > 0 && q->b[0].p && q->b[0].size > 0) {
            memset(q->b[0].p, 0, (size_t)q->b[0].size);
        }
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    }
    if (q->cmd == 2 || q->cmd == 5 || q->cmd == 6 || q->cmd == 8 || q->cmd == 13) { // Close / SetNvMapFd / SetAruid / SetGraphicsFirmwareMemoryMarginEnabled
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    }
    if (q->cmd == 3) { // Initialize -> u32 error_code = 0 + event handle
        q->out_copy = hle_new_event(NULL);
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    }
    if (q->cmd == 4) { // QueryEvent -> u32 error_code = 0 + event handle
        uint32_t w = 0;
        q->out_copy = hle_new_event(&w);
        if (w) { uint64_t one = 1; (void)write((int)w, &one, sizeof one); }
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    }
    q->out_len = 4;
    *(uint32_t *)q->out = 0;
    return RES_OK;
}
static Result nvdrv_answer(sf_req *q)   { return nvdrv_root_answer(q); }
static Result nvdrv_a_answer(sf_req *q) { return nvdrv_root_answer(q); }
static Result nvdrv_s_answer(sf_req *q) { return nvdrv_root_answer(q); }
static Result nvdrv_t_answer(sf_req *q) { return nvdrv_root_answer(q); }
SF_SERVICE("nvdrv", nvdrv_answer);
SF_SERVICE("nvdrv:a", nvdrv_a_answer);
SF_SERVICE("nvdrv:s", nvdrv_s_answer);
SF_SERVICE("nvdrv:t", nvdrv_t_answer);

