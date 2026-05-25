//
// ESP loop v8: full ActorManager scan
// v6/v7 only inspected actorList (4 empty shell ActorConfig).
// v8 dumps ALL ActorManager sub-lists to find where real actors live:
//   - actorList                @ +0x8  (DictionaryView<UInt32, ActorConfig>)  ← 4 empty
//   - updatableActorList       @ +0x10 (DictionaryView<UInt32, ActorConfig>)  ★ try this!
//   - HeroActors..SacredAnimalActors @ +0x18..+0x68 (11 TinyValueList<ActorConfig>)
//   - cacheList                @ +0x70 (DictionaryView)
//   - CampsActors              @ +0x78 (TinyValueList<ActorConfig>[] — array of lists per camp)
//   - lastFrameHerosInScene_   @ +0x80 (Int32 — should == 10 for 5v5)
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

struct Vector3 { float x, y, z; };

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

// Probe a DictionaryView<,> field: print Context dict count
static void probe_dict_view(const char *label, void *am, int off_field) {
    void *dictview = *(void **)((char *)am + off_field);
    if (!dictview) { LOGI("[esp v8] %s @+0x%x = NULL", label, off_field); return; }

    Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
    FieldInfo *f_context = il2cpp_class_get_field_from_name(dv_klass, "Context");
    int off_ctx_raw = f_context ? il2cpp_field_get_offset(f_context) : 0;
    int off_ctx = off_ctx_raw >= 0x10 ? off_ctx_raw : (off_ctx_raw + 0x10);
    void *dict = *(void **)((char *)dictview + off_ctx);
    if (!dict) { LOGI("[esp v8] %s @+0x%x dv=%p dict=NULL", label, off_field, dictview); return; }

    int count = *(int *)((char *)dict + 0x18);
    void *entries = *(void **)((char *)dict + 0x10);
    LOGI("[esp v8] %s @+0x%x dv=%p dict=%p COUNT=%d entries=%p",
         label, off_field, dictview, dict, count, entries);
}

// Probe a TinyValueList field: print size + Item_Backends ptr
static void probe_tvl(const char *label, void *am, int off_field) {
    void *tvl = *(void **)((char *)am + off_field);
    if (!tvl) { LOGI("[esp v8] %s @+0x%x = NULL", label, off_field); return; }
    // TinyValueList layout: +0x10 Item_Backends (T[] ref), +0x18 Size (int32)
    void *items = *(void **)((char *)tvl + 0x10);
    int32_t size = *(int32_t *)((char *)tvl + 0x18);
    LOGI("[esp v8] %s @+0x%x tvl=%p items=%p SIZE=%d",
         label, off_field, tvl, items, size);
}

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp v8] thread start, tid=%d", gettid());
    sleep(60);
    LOGI("[esp v8] post-warmup-60s");

    Il2CppClass *am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp v8] no ActorManager klass"); return; }

    void *am = get_singleton_instance(am_klass);
    LOGI("[esp v8] am=%p", am);
    if (!am) return;

    // Dump first 0xA0 bytes of ActorManager instance to see raw bytes
    hexdump("AM", am, 0xA0);

    LOGI("[esp v8] ===== Sub-list scan =====");
    // 2 DictionaryViews + 1 more
    probe_dict_view("actorList",          am, 0x8);
    probe_dict_view("updatableActorList", am, 0x10);
    probe_dict_view("cacheList",          am, 0x70);

    // 11 TinyValueLists
    const char *tvl_names[] = {
        "HeroActors", "OrganActors", "TowerActors", "SoldierActors",
        "DragonActors", "VehicleActors", "BuffMonsterActors", "SpringActors",
        "CallMonsterActors", "CallActors", "SacredAnimalActors"
    };
    for (int i = 0; i < 11; i++) {
        probe_tvl(tvl_names[i], am, 0x18 + i * 8);
    }

    // CampsActors is TinyValueList[] array
    void *campsArr = *(void **)((char *)am + 0x78);
    LOGI("[esp v8] CampsActors @+0x78 = %p", campsArr);
    if (campsArr) {
        uint64_t arrLen = *(uint64_t *)((char *)campsArr + 0x10);
        LOGI("[esp v8] CampsActors.length = %llu", (unsigned long long)arrLen);
        if (arrLen > 0 && arrLen < 16) {
            // Elements at +0x18 (per our v6 finding), each 8 bytes (ptr to TinyValueList)
            for (uint64_t i = 0; i < arrLen && i < 8; i++) {
                void *tvl = *(void **)((char *)campsArr + 0x18 + i * 8);
                if (tvl) {
                    int32_t size = *(int32_t *)((char *)tvl + 0x18);
                    LOGI("[esp v8] CampsActors[%llu] tvl=%p SIZE=%d", (unsigned long long)i, tvl, size);
                }
            }
        }
    }

    // lastFrameHerosInScene_ is Int32
    int lastFrameHeros = *(int *)((char *)am + 0x80);
    LOGI("[esp v8] lastFrameHerosInScene_ @+0x80 = %d", lastFrameHeros);

    LOGI("[esp v8] ========== END DIAG v8 ==========");
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
