// SPDX-License-Identifier: GPL-2.0-or-later
// The game's Options menu "Graphics" tab: the menu's fifth tab (Language Select, hidden by the
// game in this region) is shown again and opens the port's settings menu instead of the language
// screen. Its label comes from scripts/graphics_tab.py (relabelled menu text in update3.pak).
//
// Changes to the game's code (CTR NF 1.21, image offsets), visual-script nodes:
//  - CGameUtilityNodes::regionSwitch_VSC (0x10de090), used by the Options menu's root script,
//    fires none of its region outputs.
//  - igGuiPlaceableNodes::hidePlaceable_VSC (0x26f6b0): the fifth tab
//    (Comp__Menu_ButtonWithIcon005) is not hidden while an Options menu is the last menu opened.
//  - CGuiNodes::open2_VSC (0x1109200), which opens a menu project: Octane_Options_LanguageSelect
//    opens the settings menu instead. Every other project opens as before.
// The first calls of each are logged with the object names found ("Graphics tab: ...").
#include "bbport_graphics_tab.h"
#include "bbport_settings.h"
#include "common/types.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

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

/// open2_VSC's prologue (push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12).
constexpr u64 Open2 = 0x1109200;
constexpr u8 Open2Prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                  0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
/// hidePlaceable_VSC's prologue (push rbp; mov rbp,rsp; push r15; push r14; push rbx;
/// sub rsp,0x18).
constexpr u64 Hide = 0x26f6b0;
constexpr u8 HidePrologue[13] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                 0x56, 0x53, 0x48, 0x83, 0xec, 0x18};
/// The pin getter both use for their object input: (context, pin, out object), which returns
/// the object with a reference taken; the release (0x150a790) drops it.
constexpr u64 PinGetter = 0x4708a0;
constexpr u64 Release = 0x150a790;

constexpr char Project[] = "Octane_Options_LanguageSelect";
constexpr char Tab[] = "Comp__Menu_ButtonWithIcon005";

using VscFn = u64 PS4_SYSV_ABI (*)(void*, void*, void*, void*, void*, void*);
using GetterFn = void PS4_SYSV_ABI (*)(void*, void*, void**);
using ReleaseFn = void PS4_SYSV_ABI (*)(void*);

u8* image = nullptr;
VscFn open2_original = nullptr, hide_original = nullptr;
std::atomic<int> open_logged{0}, hide_logged{0};
std::mutex last_mutex;
std::string last_opened;

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

/// The node's object input (its first pin), with the getter's reference dropped again.
void* Input(void* context, void* args) {
    void* object = nullptr;
    void** pins = nullptr;
    void* pin = nullptr;
    if (args && SafeRead(static_cast<u8*>(args) + 0x10, &pins, sizeof(pins)) && pins &&
        SafeRead(pins, &pin, sizeof(pin)) && pin) {
        reinterpret_cast<GetterFn>(image + PinGetter)(context, pin, &object);
        if (object) {
            reinterpret_cast<ReleaseFn>(image + Release)(object);
        }
    }
    return object;
}

/// The object's name: the first identifier-like string a field among its first 16 points to
/// (igNamedObject::_name). `where` gets the field offset.
std::string Name(void* object, int* where = nullptr) {
    u64 fields[16];
    if (!object || !SafeRead(object, fields, sizeof(fields))) {
        return {};
    }
    for (int i = 1; i < 16; ++i) {
        char text[96] = {};
        if (fields[i] < 0x10000 || !SafeRead(reinterpret_cast<void*>(fields[i]), text, sizeof(text) - 1)) {
            continue;
        }
        std::size_t n = 0;
        while (text[n] && (std::isalnum(static_cast<unsigned char>(text[n])) || text[n] == '_')) {
            ++n;
        }
        if (n >= 4 && text[n] == 0) {
            if (where) {
                *where = i * 8;
            }
            return text;
        }
    }
    return {};
}

u64 PS4_SYSV_ABI Open2Hook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    int where = -1;
    const std::string name = Name(Input(context, args), &where);
    const bool ours = name == Project;
    if (open_logged.fetch_add(1) < 40) {
        std::printf("Graphics tab: open %s (+0x%x)%s\n", name.empty() ? "(no name)" : name.c_str(),
                    where, ours ? " -> settings menu" : "");
        std::fflush(stdout);
    }
    {
        std::scoped_lock lock{last_mutex};
        last_opened = name;
    }
    if (ours) {
        BbOverlay::OpenMenu();
        return 0;
    }
    return open2_original(context, args, a2, a3, a4, a5);
}

u64 PS4_SYSV_ABI HideHook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    int where = -1;
    const std::string name = Name(Input(context, args), &where);
    bool keep = false;
    std::string menu;
    if (name == Tab) {
        std::scoped_lock lock{last_mutex};
        menu = last_opened;
        keep = menu.empty() || menu.find("Options") != std::string::npos;
    }
    if (hide_logged.fetch_add(1) < 60 || name == Tab) {
        std::printf("Graphics tab: hide %s (+0x%x)%s%s\n", name.empty() ? "(no name)" : name.c_str(),
                    where, name == Tab ? (" after " + menu).c_str() : "",
                    keep ? " -> kept shown" : "");
        std::fflush(stdout);
    }
    if (keep) {
        return 0;
    }
    return hide_original(context, args, a2, a3, a4, a5);
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

/// The function at `offset` jumps to `hook`; the returned stub runs the moved prologue and
/// continues in the function.
VscFn Detour(u8* stub, u64 offset, const u8* prologue, std::size_t size, const void* hook) {
    std::memcpy(stub, prologue, size);
    WriteJump(stub + size, image + offset + size);
    WriteJump(image + offset, hook);
    return reinterpret_cast<VscFn>(stub);
}
} // namespace

void PatchImage(unsigned char* base, std::uint64_t size) {
    if (!BbSettings::Get().graphics_tab) {
        return;
    }
    if (Open2 + sizeof(Open2Prologue) > size ||
        std::memcmp(base + RegionQueryCall, RegionQueryCode, sizeof(RegionQueryCode)) ||
        std::memcmp(base + Open2, Open2Prologue, sizeof(Open2Prologue)) ||
        std::memcmp(base + Hide, HidePrologue, sizeof(HidePrologue))) {
        std::printf("Graphics tab: unexpected game code, off\n");
        return;
    }
    u8* stubs = static_cast<u8*>(AllocateCode(4096));
    if (!stubs) {
        std::printf("Graphics tab: no code memory, off\n");
        return;
    }
    image = base;
    open2_original = Detour(stubs, Open2, Open2Prologue, sizeof(Open2Prologue),
                            reinterpret_cast<const void*>(&Open2Hook));
    hide_original = Detour(stubs + 64, Hide, HidePrologue, sizeof(HidePrologue),
                           reinterpret_cast<const void*>(&HideHook));
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stubs, 128);
#endif
    std::memset(base + RegionQueryCall, 0x90, sizeof(RegionQueryCode));
    std::printf("Graphics tab: on (Options menu, fifth tab)\n");
}

} // namespace BbGraphicsTab
