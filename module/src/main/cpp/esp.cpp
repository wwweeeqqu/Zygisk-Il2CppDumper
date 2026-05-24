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
#define OFF_HERO_ACTORS_IN_AM     0x18
#define OFF_POSITION_IN_LINKER    0x4C4
#define OFF_OBJID_IN_LINKER       0x4AC
#define OFF_LOGIC_VISIBLE         0x511
#define OFF_IN_CAMERA             0x513
#define OFF_FORWARD_IN_LINKER     0x4B8

struct Vector3 { float x, y, z; };

static const Il2CppImage *find_assembly_csharp() {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) { LOGE("[esp] no domain"); return nullptr; }
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    LOGI("[esp] %zu assemblies", n);
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
    sleep(20);
    LOGI("[esp] post-warmup");

    const Il2CppImage *img = find_assembly_csharp();
    if (!img) { LOGE("[esp] no image, ESP aborted"); return; }

    Il2CppClass *am_klass = il2cpp_class_from_name(img, "Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp] no ActorManager class"); return; }
    LOGI("[esp] ActorManager class: %p", am_klass);

    Il2CppClass *singleton_klass = il2cpp_class_get_parent(am_klass);
    if (!singleton_klass) { LOGE("[esp] no parent (Singleton)"); return; }

    FieldInfo *s_inst_field = il2cpp_class_get_field_from_name(singleton_klass, "s_instance");
    if (!s_inst_field) s_inst_field = il2cpp_class_get_field_from_name(am_klass, "s_instance");
    if (!s_inst_field) { LOGE("[esp] s_instance field not found"); return; }
    LOGI("[esp] s_instance field: %p (offset 0x%zx)", s_inst_field, il2cpp_field_get_offset(s_inst_field));

    void *am_instance = nullptr;
    il2cpp_field_static_get_value(s_inst_field, &am_instance);
    LOGI("[esp] initial s_instance = %p", am_instance);

    FieldInfo *hero_actors_field = il2cpp_class_get_field_from_name(am_klass, "HeroActors");
    if (!hero_actors_field) { LOGE("[esp] HeroActors field not found"); return; }
    size_t ha_offset = il2cpp_field_get_offset(hero_actors_field);
    LOGI("[esp] HeroActors field offset = 0x%zx (expected 0x18)", ha_offset);

    int iter = 0;
    while (true) {
        sleep(1);
        iter++;

        il2cpp_field_static_get_value(s_inst_field, &am_instance);
        if (!am_instance) {
            if (iter % 10 == 0) LOGI("[esp] iter %d: instance NULL (not in match?)", iter);
            continue;
        }

        char *ha_ptr = (char *)am_instance + ha_offset;
        void *array = *(void **)ha_ptr;

        if (!array) {
            if (iter % 10 == 0) LOGI("[esp] iter %d: HeroActors.array NULL", iter);
            continue;
        }

        // IL2CPP array: [klass* 8B][monitor 8B][bounds* 8B][max_length 8B][elements...]
        uint64_t size = *(uint64_t *)((char *)array + 0x18);

        if (iter % 5 == 0) LOGI("[esp] iter %d: am=%p array=%p size=%llu",
                                iter, am_instance, array, (unsigned long long)size);

        if (size == 0 || size > 64) continue;

        void **elements = (void **)((char *)array + 0x20);
        for (uint64_t i = 0; i < size && i < 16; i++) {
            void *linker = elements[i];
            if (!linker) continue;
            uint32_t objId = *(uint32_t *)((char *)linker + OFF_OBJID_IN_LINKER);
            Vector3 *pos = (Vector3 *)((char *)linker + OFF_POSITION_IN_LINKER);
            uint8_t visible = *((uint8_t *)linker + OFF_LOGIC_VISIBLE);
            uint8_t in_cam  = *((uint8_t *)linker + OFF_IN_CAMERA);
            LOGI("[esp]   hero[%llu] obj=%u pos=(%.1f,%.1f,%.1f) vis=%d cam=%d",
                 (unsigned long long)i, objId, pos->x, pos->y, pos->z, visible, in_cam);
        }
    }
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
