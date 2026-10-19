// SPDX-License-Identifier: GPL-2.0-or-later
// The game's Options menu "Graphics" tab: the menu's fifth tab (Language Select, hidden by the
// game in this region) is shown again and opens the port's settings menu instead of the language
// screen. Its label comes from scripts/graphics_tab.py (a relabelled menu archive).
//
// Two changes to the game's code (CTR NF 1.21, image offsets):
//  - CGameUtilityNodes::regionSwitch_VSC (0x10de090), used only by the Options menu's root
//    script, fires none of its region outputs: the script's branch that hides the fifth tab
//    (Comp__Menu_ButtonWithIcon005, igGuiPlaceableNodes::hidePlaceable_VSC) is not taken.
//  - CGuiNodes::open2_VSC (0x1109200), the visual-script node that opens a menu project: when
//    the project is Octane_Options_LanguageSelect, the settings menu opens and the game's call
//    returns without opening anything. Every other project opens as before.
#include "bbport_graphics_tab.h"
#include "bbport_settings.h"
#include "common/types.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace BbOverlay {
void OpenMenu(); // bbport_overlay.h (Vulkan headers)
}

namespace BbGraphicsTab {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// regionSwitch_VSC: `call 0x1036010` (the region query filling its three output flags).
constexpr u64 RegionQueryCall = 0x10de0c9;
constexpr u8 RegionQueryCode[5] = {0xe8, 0x42, 0x7f, 0xf5, 0xff};

/// open2_VSC's prologue (push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12), moved
/// into the continuation stub.
constexpr u64 Open2 = 0x1109200;
constexpr u8 Open2Prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                  0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
/// The pin getter open2_VSC uses for its project input: (context, pin, out object).
constexpr u64 PinGetter = 0x4708a0;

constexpr char Project[] = "Octane_Options_LanguageSelect";

using Open2Fn = u64 PS4_SYSV_ABI (*)(void*, void*, void*, void*, void*, void*);
using GetterFn = void PS4_SYSV_ABI (*)(void*, void*, void**);

u8* image = nullptr;
Open2Fn original = nullptr;
std::atomic<int> logged{0};

/// Copies `size` bytes from `address`, false when they are not readable.
bool SafeRead(const void* address, void* out, std::size_t size) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        static_cast<const u8*>(address) + size >
            static_cast<const u8*>(mbi.BaseAddress) + mbi.RegionSize) {
        return false;
    }
    std::memcpy(out, address, size);
    return true;
#else
    iovec local{out, size}, remote{const_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
#endif
}

/// The project's name: a string pointer among the object's first fields (igNamedObject::_name).
bool IsProject(void* object, char* name, std::size_t name_size) {
    name[0] = 0;
    if (!object) {
        return false;
    }
    u64 fields[8];
    if (!SafeRead(object, fields, sizeof(fields))) {
        return false;
    }
    for (int i = 1; i < 8; ++i) {
        char text[64] = {};
        if (fields[i] < 0x10000 || !SafeRead(reinterpret_cast<void*>(fields[i]), text, sizeof(text) - 1)) {
            continue;
        }
        if (std::strncmp(text, "Octane_", 7) && std::strncmp(text, "common_", 7) &&
            std::strncmp(text, "PauseMenu", 9)) {
            continue;
        }
        std::snprintf(name, name_size, "%s", text);
        return std::strcmp(text, Project) == 0;
    }
    return false;
}

u64 PS4_SYSV_ABI Open2Hook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    void* project = nullptr;
    void** pins = nullptr;
    if (args && SafeRead(static_cast<u8*>(args) + 0x10, &pins, sizeof(pins)) && pins) {
        void* pin = nullptr;
        if (SafeRead(pins, &pin, sizeof(pin)) && pin) {
            reinterpret_cast<GetterFn>(image + PinGetter)(context, pin, &project);
        }
    }
    char name[64];
    const bool ours = IsProject(project, name, sizeof(name));
    if (logged.fetch_add(1) < 24) {
        std::printf("Graphics tab: menu open %s%s\n", name[0] ? name : "(unnamed)",
                    ours ? " -> settings menu" : "");
        std::fflush(stdout);
    }
    if (ours) {
        BbOverlay::OpenMenu();
        return 0;
    }
    return original(context, args, a2, a3, a4, a5);
}

void* AllocateCode(std::size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

/// movabs rax, target; jmp rax (12 bytes).
void WriteJump(u8* at, const void* target) {
    at[0] = 0x48;
    at[1] = 0xb8;
    const u64 t = reinterpret_cast<u64>(target);
    std::memcpy(at + 2, &t, 8);
    at[10] = 0xff;
    at[11] = 0xe0;
}
} // namespace

void PatchImage(unsigned char* base, std::uint64_t size) {
    if (!BbSettings::Get().graphics_tab) {
        return;
    }
    if (Open2 + sizeof(Open2Prologue) > size ||
        std::memcmp(base + RegionQueryCall, RegionQueryCode, sizeof(RegionQueryCode)) ||
        std::memcmp(base + Open2, Open2Prologue, sizeof(Open2Prologue))) {
        std::printf("Graphics tab: unexpected game code, off\n");
        return;
    }
    u8* stub = static_cast<u8*>(AllocateCode(4096));
    if (!stub) {
        std::printf("Graphics tab: no code memory, off\n");
        return;
    }
    image = base;
    // The continuation: the moved prologue, then back into open2_VSC past it.
    std::memcpy(stub, Open2Prologue, sizeof(Open2Prologue));
    WriteJump(stub + sizeof(Open2Prologue), base + Open2 + sizeof(Open2Prologue));
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
#endif
    original = reinterpret_cast<Open2Fn>(stub);
    WriteJump(base + Open2, reinterpret_cast<const void*>(&Open2Hook));
    // The region query is skipped: its three flags stay false, no output fires.
    std::memset(base + RegionQueryCall, 0x90, sizeof(RegionQueryCode));
    std::printf("Graphics tab: on (Options menu, fifth tab)\n");
}

} // namespace BbGraphicsTab
