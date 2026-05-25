//
// ESP loop v10: correct ActorConfig→ActorConfigInner→ActorLinker→position chain
// v9 finding (the bug): inner (+0x50 on ActorConfig) is ActorConfigInner, NOT ActorLinker.
//   ActorConfigInner.actorLinker @ +0x8 is the real ActorLinker.
//
// Verified chain (dump.cs):
//   ActorConfig.inner   @ +0x50   → ActorConfigInner instance
//   ActorConfigInner.actorLinker @ +0x8   → ActorLinker instance
//   ActorLinker.ObjID   @ +0x4AC  (UInt32)
//   ActorLinker.position @ +0x4C4 (Vector3, 12 bytes)
//   ActorLinker.rotation @ +0x4D0 (Quaternion, 16 bytes)
//   ActorLinker.name    @ +0x4B0  (String ref)
//

#include "esp.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"
#include <thread>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#define DO_API(r, n, p) extern r (*n) p

#include "il2cpp-api-functions.h"

#undef DO_API

#define OFF_AC_ACTORTYPE   0x18
#define OFF_AC_CONFIGID    0x1c
#define OFF_AC_CMPTYPE     0x20
#define OFF_AC_BATTLEORDER 0x24
#define OFF_AC_INNER       0x50

#define OFF_INNER_LINKER   0x8

#define OFF_AL_OBJID       0x4AC
#define OFF_AL_NAME        0x4B0
#define OFF_AL_FORWARD     0x4B8
#define OFF_AL_POSITION    0x4C4
#define OFF_AL_ROTATION    0x4D0

struct Vector3 { float x, y, z; };
struct VInt3   { int x, y, z; };

static void hexdump(const char *label, const void *p, size_t len) {
    if (!p) { LOGI("[esp] %s: NULL", label); return; }
    char line[256];
    for (size_t off = 0; off < len; off += 16) {
        int pos = snprintf(line, sizeof(line), "[esp] %s+%03zx:", label, off);
        for (size_t j = 0; j < 16 && off+j < len; j++) {
            pos += snprintf(line+pos, sizeof(line)-pos, " %02x", ((const uint8_t*)p)[off+j]);
        }
        LOGI("%s", line);
    }
}

static Il2CppClass *find_class_anywhere(const char *ns, const char *name) {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass *k = il2cpp_class_from_name(img, ns, name);
        if (k) return k;
    }
    return nullptr;
}

static void *get_singleton_instance(Il2CppClass *klass) {
    if (!klass) return nullptr;
    Il2CppClass *parent = il2cpp_class_get_parent(klass);
    FieldInfo *f = il2cpp_class_get_field_from_name(parent, "s_instance");
    if (!f) f = il2cpp_class_get_field_from_name(klass, "s_instance");
    if (!f) return nullptr;
    void *inst = nullptr;
    il2cpp_field_static_get_value(f, &inst);
    return inst;
}

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp v10] thread start, tid=%d", gettid());
    sleep(60);
    LOGI("[esp v10] post-warmup-60s");

    Il2CppClass *am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp v10] no ActorManager klass"); return; }

    void *am = get_singleton_instance(am_klass);
    if (!am) { LOGE("[esp v10] am NULL"); return; }
    LOGI("[esp v10] am=%p", am);

    void *dictview = *(void **)((char *)am + 0x10);  // updatableActorList
    if (!dictview) { LOGE("[esp v10] dictview NULL"); return; }

    Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
    FieldInfo *f_context = il2cpp_class_get_field_from_name(dv_klass, "Context");
    int off_ctx_raw = f_context ? il2cpp_field_get_offset(f_context) : 0;
    int off_ctx = off_ctx_raw >= 0x10 ? off_ctx_raw : (off_ctx_raw + 0x10);
    void *dict = *(void **)((char *)dictview + off_ctx);
    if (!dict) { LOGE("[esp v10] dict NULL"); return; }

    int count = *(int *)((char *)dict + 0x18);
    void *entries = *(void **)((char *)dict + 0x10);
    LOGI("[esp v10] updatableActorList COUNT=%d entries=%p", count, entries);
    if (!entries || count <= 0 || count > 256) return;

    int dumped = 0;
    int iter_cap = count < 30 ? count : 30;

    for (int i = 0; i < iter_cap; i++) {
        char *e = (char *)entries + 0x18 + i * 24;
        int hashCode = *(int *)(e + 0);
        int next     = *(int *)(e + 4);
        uint32_t key = *(uint32_t *)(e + 8);
        void *ac     = *(void **)(e + 16);

        if (hashCode < 0 && next < 0) continue;
        if (!ac) continue;

        int actorType = *(int *)((char *)ac + OFF_AC_ACTORTYPE);
        int configId  = *(int *)((char *)ac + OFF_AC_CONFIGID);
        int cmpType   = *(int *)((char *)ac + OFF_AC_CMPTYPE);
        int battleOrder = *(int *)((char *)ac + OFF_AC_BATTLEORDER);
        void *inner   = *(void **)((char *)ac + OFF_AC_INNER);

        if (!inner) {
            LOGI("[esp v10] [%d] key=%u Type=%d Cfg=%d Camp=%d BO=%d inner=NULL",
                 i, key, actorType, configId, cmpType, battleOrder);
            continue;
        }

        // ★ FIX: inner is ActorConfigInner, ActorLinker is at inner+0x8
        void *actor_linker = *(void **)((char *)inner + OFF_INNER_LINKER);
        if (!actor_linker) {
            LOGI("[esp v10] [%d] key=%u Type=%d Cfg=%d Camp=%d BO=%d inner=%p AL=NULL",
                 i, key, actorType, configId, cmpType, battleOrder, inner);
            continue;
        }

        uint32_t objId  = *(uint32_t *)((char *)actor_linker + OFF_AL_OBJID);
        Vector3 *pos    = (Vector3 *)((char *)actor_linker + OFF_AL_POSITION);
        VInt3   *fwd    = (VInt3   *)((char *)actor_linker + OFF_AL_FORWARD);

        LOGI("[esp v10] [%d] key=%u Type=%d Cfg=%d Camp=%d BO=%d AL=%p ObjID=%u pos=(%.1f,%.1f,%.1f) fwd=(%d,%d,%d)",
             i, key, actorType, configId, cmpType, battleOrder,
             actor_linker, objId, pos->x, pos->y, pos->z, fwd->x, fwd->y, fwd->z);

        // hexdump first 2 AL position window for verification
        if (dumped < 2) {
            char label[48];
            snprintf(label, sizeof(label), "ACI_%u", key);
            hexdump(label, inner, 0x30);
            snprintf(label, sizeof(label), "AL_%u_pos", key);
            hexdump(label, (char *)actor_linker + 0x4A0, 0x40);
            dumped++;
        }
    }

    LOGI("[esp v10] ========== END DIAG v10 ==========");
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
