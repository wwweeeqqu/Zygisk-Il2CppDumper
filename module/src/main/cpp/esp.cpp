//
// ESP loop: read sgame ActorManager.HeroActors -> ActorLinker -> position
// Based on dump.cs ground truth (HeroActors @ +0x18, ActorLinker.position @ +0x4C4)
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

// Ground truth from dump.cs (sgame v11.3.1.1)
#define OFF_HERO_ACTORS_IN_AM     0x18    // ActorManager.HeroActors
#define OFF_POSITION_IN_LINKER    0x4C4   // ActorLinker.position (Vector3)
#define OFF_OBJID_IN_LINKER       0x4AC   // ActorLinker.ObjID (UInt32)
#define OFF_LOGIC_VISIBLE         0x511   // ActorLinker._logicVisible (Boolean)
#define OFF_IN_CAMERA             0x513   // ActorLinker._inCamera (Boolean)
#define OFF_FORWARD_IN_LINKER     0x4B8   // ActorLinker.forward (VInt3)

struct Vector3 { float x, y, z; };

// helper: find Assembly-CSharp image
static const Il2CppImage *find_assembly_csharp() {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) { LOGE("[esp] no domain"); return nullptr; }
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    LOGI("[esp] %zu assemblies", n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        const char *name = il2cpp_image_get_name(img);
        if (name && (strstr(name, "Assembly-CSharp") || strstr(name, "Scripts.GameCore"))) {
            LOGI("[esp] candidate image: %s", name);
        }
    }
    // search for ActorManager in EACH image
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass *k = il2cpp_class_from_name(img, "Assets.Scripts.GameLogic", "ActorManager");
        if (k) {
            const char *imgname = il2cpp_image_get_name(img);
            LOGI("[esp] ActorManager found in image: %s", imgname);
            return img;
        }
    }
    LOGE("[esp] ActorManager not found in any image");
    return nullptr;
}

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp] thread start, tid=%d", gettid());

    // Wait extra for game stabilization
    sleep(20);
    LOGI("[esp] post-warmup");

    const Il2CppImage *img = find_assembly_csharp();
    if (!img) {
        LOGE("[esp] no image, ESP aborted");
        return;
    }

    Il2CppClass *am_klass = il2cpp_class_from_name(img, "Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp] no ActorManager class"); return; }
    LOGI("[esp] ActorManager class: %p", am_klass);

    // Print parent class chain
    Il2CppClass *parent = il2cpp_class_get_parent(am_klass);
    while (parent) {
        const char *pname = il2cpp_class_get_name(parent);
        const char *pns = il2cpp_class_get_namespace(parent);
        LOGI("[esp]   parent: %s.%s", pns?pns:"", pname?pname:"");
        parent = il2cpp_class_get_parent(parent);
    }

    // Try Singleton<ActorManager>.s_instance (parent of ActorManager)
    Il2CppClass *singleton_klass = il2cpp_class_get_parent(am_klass);
    if (!singleton_klass) { LOGE("[esp] no parent (Singleton)"); return; }

    // Try to get static field s_instance from parent Singleton<ActorManager>
    FieldInfo *s_inst_field = il2cpp_class_get_field_from_name(singleton_klass, "s_instance");
    if (!s_inst_field) {
        LOGI("[esp] s_instance not on Singleton parent, try ActorManager itself");
        s_inst_field = il2cpp_class_get_field_from_name(am_klass, "s_instance");
    }
    if (!s_inst_field) {
        LOGE("[esp] s_instance field not found anywhere");
        return;
    }
    LOGI("[esp] s_instance field: %p (offset 0x%zx)", s_inst_field, il2cpp_field_get_offset(s_inst_field));

    // Read static field value (pointer to ActorManager instance)
    void *am_instance = nullptr;
    il2cpp_field_static_get_value(s_inst_field, &am_instance);
    LOGI("[esp] initial s_instance = %p", am_instance);

    // Find HeroActors field offset (should be 0x18 from dump.cs)
    FieldInfo *hero_actors_field = il2cpp_class_get_field_from_name(am_klass, "HeroActors");
    if (!hero_actors_field) { LOGE("[esp] HeroActors field not found"); return; }
    size_t ha_offset = il2cpp_field_get_offset(hero_actors_field);
    LOGI("[esp] HeroActors field offset = 0x%zx (expected 0x18)", ha_offset);

    // Main loop: read positions
    int iter = 0;
    while (true) {
        sleep(1);
        iter++;

        // re-read singleton instance (may have changed if game restarted)
        il2cpp_field_static_get_value(s_inst_field, &am_instance);
        if (!am_instance) {
            if (iter % 10 == 0) LOGI("[esp] iter %d: instance NULL (not in match?)", iter);
            continue;
        }

        // HeroActors is at am_instance + ha_offset
        // TinyValueList<ActorLinker> struct layout: { ActorLinker[] Item_Backends @ +0; int Size @ +8 }
        char *ha_ptr = (char *)am_instance + ha_offset;
        void **item_backends_ref = (void **)ha_ptr;
        void *array = *item_backends_ref;
        int32_t size = *(int32_t *)(ha_ptr + 8);

        if (iter % 5 == 0) LOGI("[esp] iter %d: am=%p HeroActors.array=%p size=%d",
                                iter, am_instance, array, size);

        if (!array || size <= 0 || size > 64) continue;

        // IL2CPP array layout: [Il2CppObject header 16B][bounds 8B][size 8B][elements...]
        // For arrays of references, element size = 8 bytes
        // Elements start at offset 0x20 (32) from array start
        void **elements = (void **)((char *)array + 0x20);
        for (int32_t i = 0; i < size && i < 16; i++) {
            void *linker = elements[i];
            if (!linker) continue;
            uint32_t objId = *(uint32_t *)((char *)linker + OFF_OBJID_IN_LINKER);
            Vector3 *pos = (Vector3 *)((char *)linker + OFF_POSITION_IN_LINKER);
            uint8_t visible = *((uint8_t *)linker + OFF_LOGIC_VISIBLE);
            uint8_t in_cam  = *((uint8_t *)linker + OFF_IN_CAMERA);
            LOGI("[esp]   hero[%d] obj=%u pos=(%.1f,%.1f,%.1f) vis=%d cam=%d",
                 i, objId, pos->x, pos->y, pos->z, visible, in_cam);
        }
    }
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
