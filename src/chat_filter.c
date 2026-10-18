#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* =========================================================================
 * System msimg32.dll proxy forwarding
 * ========================================================================= */
static HMODULE g_real_msimg32 = NULL;
static FARPROC g_pAlphaBlend = NULL;
static FARPROC g_pDllInitialize = NULL;
static FARPROC g_pGradientFill = NULL;
static FARPROC g_pTransparentBlt = NULL;
static FARPROC g_pVSetDdrawflag = NULL;

static void LoadRealMsimg32(void) {
    if (g_real_msimg32) return;
    char sys_dir[MAX_PATH];
    if (GetSystemDirectoryA(sys_dir, MAX_PATH)) {
        char full_path[MAX_PATH];
        _snprintf_s(full_path, sizeof(full_path), _TRUNCATE, "%s\\msimg32.dll", sys_dir);
        g_real_msimg32 = LoadLibraryA(full_path);
        if (g_real_msimg32) {
            g_pAlphaBlend = GetProcAddress(g_real_msimg32, "AlphaBlend");
            g_pDllInitialize = GetProcAddress(g_real_msimg32, "DllInitialize");
            g_pGradientFill = GetProcAddress(g_real_msimg32, "GradientFill");
            g_pTransparentBlt = GetProcAddress(g_real_msimg32, "TransparentBlt");
            g_pVSetDdrawflag = GetProcAddress(g_real_msimg32, "vSetDdrawflag");
        }
    }
}

__declspec(naked) void Proxy_AlphaBlend(void) {
    __asm { jmp dword ptr [g_pAlphaBlend] }
}
__declspec(naked) void Proxy_DllInitialize(void) {
    __asm { jmp dword ptr [g_pDllInitialize] }
}
__declspec(naked) void Proxy_GradientFill(void) {
    __asm { jmp dword ptr [g_pGradientFill] }
}
__declspec(naked) void Proxy_TransparentBlt(void) {
    __asm { jmp dword ptr [g_pTransparentBlt] }
}
__declspec(naked) void Proxy_vSetDdrawflag(void) {
    __asm { jmp dword ptr [g_pVSetDdrawflag] }
}

/* =========================================================================
 * BC2 Client constants (exact Retail/Rome build)
 * ========================================================================= */
#define ADDR_UI_SINGLETON_PTR        0x019A4CA8u
#define OFFSET_UI_EVENT_SUBOBJECT    0x00082AF8u
#define OFFSET_UI_TEXTBOX_MGR_PTR    0x00082D0Cu

#define VTABLE_UI_ROOT_EXPECTED      0x01440830u
#define VTABLE_UI_EVENT_EXPECTED     0x01440804u
#define FN_ON_UI_EVENT_EXPECTED      0x009640D0u

#define VTABLE_CHAT_INPUT_EXPECTED   0x0142A3D0u
#define FN_ON_CHAT_INPUT_EXPECTED    0x00751380u

#define VTABLE_INGAME_HUD_SUB        0x0142B794u
#define FN_ON_INGAME_HUD_EVENT       0x007875B0u

#define VTABLE_MSG_CHAT              0x0141C038u
#define FN_MSG_CHAT_CTOR             0x005FA170u
#define ADDR_EMPTY_STRING_BUF        0x0154C450u
#define ADDR_EMPTY_STRING_CAP        0x0154C451u

/* =========================================================================
 * Mute list state & file I/O
 * ========================================================================= */
#define MAX_MUTED_PLAYERS 256
#define MAX_NAME_LEN      64

static CRITICAL_SECTION g_cs;
static char g_dll_dir[MAX_PATH];
static char g_mute_file[MAX_PATH];
static char g_log_file[MAX_PATH];
static char g_muted[MAX_MUTED_PLAYERS][MAX_NAME_LEN];
static int  g_muted_count = 0;
static FILETIME g_last_mtime = {0, 0};
static DWORD g_last_file_check_tick = 0;
static volatile LONG g_running = 1;
static HANDLE g_worker_thread = NULL;

static void WriteLog(const char* fmt, ...) {
    if (!g_log_file[0]) return;
    FILE* f = NULL;
    if (fopen_s(&f, g_log_file, "a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
}

static void TrimInPlace(char* s) {
    char* p = s;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static int StrContainsCaseInsensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        if (_strnicmp(p, needle, nlen) == 0) return 1;
    }
    return 0;
}

static void LoadMutedListLocked(int force) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(g_mute_file, GetFileExInfoStandard, &fad)) {
        if (force) {
            FILE* f = NULL;
            if (fopen_s(&f, g_mute_file, "w") == 0 && f) {
                fprintf(f, "# BC2 Chat Filter - Muted players / keywords (one per line, case-insensitive)\n");
                fprintf(f, "# In-game chat commands: /mute <name>, /unmute <name>, /mutelist\n");
                fclose(f);
            }
            g_muted_count = 0;
        }
        return;
    }

    if (!force &&
        fad.ftLastWriteTime.dwLowDateTime == g_last_mtime.dwLowDateTime &&
        fad.ftLastWriteTime.dwHighDateTime == g_last_mtime.dwHighDateTime) {
        return;
    }
    g_last_mtime = fad.ftLastWriteTime;

    FILE* f = NULL;
    if (fopen_s(&f, g_mute_file, "r") != 0 || !f) return;

    g_muted_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        TrimInPlace(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (g_muted_count < MAX_MUTED_PLAYERS) {
            strncpy_s(g_muted[g_muted_count], MAX_NAME_LEN, line, _TRUNCATE);
            g_muted_count++;
        }
    }
    fclose(f);
    WriteLog("Loaded %d muted entries from muted_players.txt", g_muted_count);
}

static void SaveMutedListLocked(void) {
    FILE* f = NULL;
    if (fopen_s(&f, g_mute_file, "w") != 0 || !f) return;
    fprintf(f, "# BC2 Chat Filter - Muted players / keywords (one per line, case-insensitive)\n");
    fprintf(f, "# In-game chat commands: /mute <name>, /unmute <name>, /mutelist\n");
    for (int i = 0; i < g_muted_count; i++) {
        fprintf(f, "%s\n", g_muted[i]);
    }
    fclose(f);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(g_mute_file, GetFileExInfoStandard, &fad)) {
        g_last_mtime = fad.ftLastWriteTime;
    }
}

static void MaybeReloadMutedListLocked(void) {
    DWORD now = GetTickCount();
    if (now - g_last_file_check_tick >= 2000) {
        g_last_file_check_tick = now;
        LoadMutedListLocked(0);
    }
}

static int IsSenderOrMessageMuted(const char* sender, const char* first_word) {
    int blocked = 0;
    EnterCriticalSection(&g_cs);
    MaybeReloadMutedListLocked();
    for (int i = 0; i < g_muted_count; i++) {
        const char* pat = g_muted[i];
        if (!pat[0]) continue;
        if (sender && StrContainsCaseInsensitive(sender, pat)) {
            blocked = 1;
            break;
        }
        if (first_word && StrContainsCaseInsensitive(first_word, pat)) {
            blocked = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return blocked;
}

/* =========================================================================
 * Local Chat Notification Helper (displays feedback only to local player)
 * ========================================================================= */
typedef struct {
    const char* mpBegin;
    const char* mpEnd;
    const char* mpCapacity;
} EastlString8;

typedef struct {
    const wchar_t* mpBegin;
    const wchar_t* mpEnd;
    const wchar_t* mpCapacity;
} EastlString16;

typedef void (__fastcall *FnOnUiEvent)(
    void* this_ptr,
    void* edx_unused,
    int cat,
    int type,
    void* a3,
    void* msg_ptr
);

static FnOnUiEvent g_orig_OnUiEvent = (FnOnUiEvent)FN_ON_UI_EVENT_EXPECTED;

static void ShowLocalChatFeedback(const char* text) {
    uintptr_t ui_root = *(uintptr_t*)ADDR_UI_SINGLETON_PTR;
    if (!ui_root) return;

    unsigned char msg_buf[0x90];
    memset(msg_buf, 0, sizeof(msg_buf));

    const char* tag = "[ChatFilter]";
    EastlString8 s_word;
    s_word.mpBegin = text;
    s_word.mpEnd = text + strlen(text);
    s_word.mpCapacity = s_word.mpEnd + 1;

    EastlString8 s_sender;
    s_sender.mpBegin = tag;
    s_sender.mpEnd = tag + strlen(tag);
    s_sender.mpCapacity = s_sender.mpEnd + 1;

    /* Instead of calling engine allocator, construct a lightweight fake msg struct
     * that matches exact layout read by 0x009648BA!
     * Let's check what 0x009648BA reads from msg (EDI):
     *   [EDI + 0x20]: begin of EastlString8 vector (1 element = 12 bytes)
     *   [EDI + 0x24]: end of EastlString8 vector
     *   [EDI + 0x60]: float display_time (5.0f)
     *   [EDI + 0x68]: int chat_group (0 = All)
     *   [EDI + 0x6C]: EastlString8 sender_name (mpBegin, mpEnd, mpCapacity)
     */
    EastlString8 word_vec[1];
    word_vec[0] = s_word;

    *(uint32_t*)(msg_buf + 0x00) = VTABLE_MSG_CHAT;
    *(uint32_t*)(msg_buf + 0x20) = (uint32_t)&word_vec[0];
    *(uint32_t*)(msg_buf + 0x24) = (uint32_t)&word_vec[1];
    *(uint32_t*)(msg_buf + 0x28) = (uint32_t)&word_vec[1];
    *(float*)   (msg_buf + 0x60) = 6.0f;
    *(uint32_t*)(msg_buf + 0x68) = 0; /* TextChatGroup_All */
    *(EastlString8*)(msg_buf + 0x6C) = s_sender;

    void* subobj = (void*)(ui_root + OFFSET_UI_EVENT_SUBOBJECT);
    g_orig_OnUiEvent(subobj, NULL, 0x21, 0x1A, NULL, msg_buf);
}

/* =========================================================================
 * Hook 1: Incoming UI Event Dispatcher (0x009640D0 on vtable 0x01440804)
 * ========================================================================= */
static uint32_t g_clone_vtable_ui_event[8];

static void __fastcall Hooked_OnUiEvent(
    void* this_ptr,
    void* edx_unused,
    int cat,
    int type,
    void* a3,
    void* msg_ptr
) {
    (void)edx_unused;
    /* Chat messages arrive as cat == 0x21 (33), type == 0x1A (26) (or 0x11/0x12) */
    if (cat == 0x21 && (type == 0x1A || type == 0x11 || type == 0x12) && msg_ptr != NULL) {
        const char* sender = NULL;
        const char* first_word = NULL;

        __try {
            uintptr_t m = (uintptr_t)msg_ptr;
            sender = *(const char**)(m + 0x6C);
            uintptr_t vec_begin = *(uintptr_t*)(m + 0x20);
            uintptr_t vec_end   = *(uintptr_t*)(m + 0x24);
            if (vec_begin && vec_end > vec_begin) {
                first_word = *(const char**)vec_begin;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sender = NULL;
            first_word = NULL;
        }

        if (sender && strcmp(sender, "[ChatFilter]") != 0) {
            if (IsSenderOrMessageMuted(sender, first_word)) {
                WriteLog("Blocked chat from '%s': '%s'",
                         sender ? sender : "",
                         first_word ? first_word : "");
                return;
            }
        }
    }

    g_orig_OnUiEvent(this_ptr, NULL, cat, type, a3, msg_ptr);
}

/* =========================================================================
 * Hook 2: In-Game HUD Subobject Dispatcher (0x007875B0 on vtable 0x0142B794)
 *   Catches cat == 0x21, type == 0x11 / 0x12 / 0x10 / 0x19 if routed to HUD
 * ========================================================================= */
typedef void (__fastcall *FnOnHudEvent)(
    void* this_ptr,
    void* edx_unused,
    int cat,
    int type,
    void* a3,
    void* msg_ptr
);

static FnOnHudEvent g_orig_OnHudEvent = (FnOnHudEvent)FN_ON_INGAME_HUD_EVENT;
static uint32_t g_clone_vtable_hud_event[8];

static void __fastcall Hooked_OnHudEvent(
    void* this_ptr,
    void* edx_unused,
    int cat,
    int type,
    void* a3,
    void* msg_ptr
) {
    (void)edx_unused;
    if (cat == 0x21 && (type == 0x11 || type == 0x12 || type == 0x1A) && msg_ptr != NULL) {
        const char* sender = NULL;
        const char* first_word = NULL;
        __try {
            uintptr_t m = (uintptr_t)msg_ptr;
            sender = *(const char**)(m + 0x6C);
            uintptr_t vec_begin = *(uintptr_t*)(m + 0x20);
            uintptr_t vec_end   = *(uintptr_t*)(m + 0x24);
            if (vec_begin && vec_end > vec_begin) {
                first_word = *(const char**)vec_begin;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sender = NULL;
            first_word = NULL;
        }

        if (sender && IsSenderOrMessageMuted(sender, first_word)) {
            WriteLog("Blocked HUD chat from '%s'", sender);
            return;
        }
    }
    g_orig_OnHudEvent(this_ptr, NULL, cat, type, a3, msg_ptr);
}

/* =========================================================================
 * Hook 3: Outgoing Chat Submit (0x00751380 on vtable 0x0142A3D0)
 *   Handles /mute <name>, /unmute <name>, /mutelist in-game commands
 * ========================================================================= */
typedef void (__fastcall *FnOnChatInputSubmit)(
    void* this_ptr,
    void* edx_unused,
    int channel_id,
    EastlString16* wtext
);

static FnOnChatInputSubmit g_orig_OnChatInputSubmit = (FnOnChatInputSubmit)FN_ON_CHAT_INPUT_EXPECTED;
static uint32_t g_clone_vtable_chat_input[8];

static int HandleSlashCommand(const char* utf8_cmd) {
    while (*utf8_cmd == ' ' || *utf8_cmd == '\t') utf8_cmd++;
    if (utf8_cmd[0] != '/') return 0;

    if (_strnicmp(utf8_cmd, "/mute ", 6) == 0) {
        char arg[MAX_NAME_LEN];
        strncpy_s(arg, sizeof(arg), utf8_cmd + 6, _TRUNCATE);
        TrimInPlace(arg);
        if (!arg[0]) {
            ShowLocalChatFeedback("Usage: /mute <player_or_word>");
            return 1;
        }
        EnterCriticalSection(&g_cs);
        LoadMutedListLocked(0);
        int exists = 0;
        for (int i = 0; i < g_muted_count; i++) {
            if (_stricmp(g_muted[i], arg) == 0) {
                exists = 1;
                break;
            }
        }
        char feedback[128];
        if (exists) {
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Already muted: %s", arg);
        } else if (g_muted_count < MAX_MUTED_PLAYERS) {
            strncpy_s(g_muted[g_muted_count++], MAX_NAME_LEN, arg, _TRUNCATE);
            SaveMutedListLocked();
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Muted: %s", arg);
            WriteLog("User muted via command: '%s'", arg);
        } else {
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Mute list is full!");
        }
        LeaveCriticalSection(&g_cs);
        ShowLocalChatFeedback(feedback);
        return 1;
    }

    if (_strnicmp(utf8_cmd, "/unmute ", 8) == 0) {
        char arg[MAX_NAME_LEN];
        strncpy_s(arg, sizeof(arg), utf8_cmd + 8, _TRUNCATE);
        TrimInPlace(arg);
        if (!arg[0]) {
            ShowLocalChatFeedback("Usage: /unmute <player_or_word>");
            return 1;
        }
        EnterCriticalSection(&g_cs);
        LoadMutedListLocked(0);
        int removed = 0;
        for (int i = 0; i < g_muted_count; ) {
            if (_stricmp(g_muted[i], arg) == 0 || StrContainsCaseInsensitive(g_muted[i], arg)) {
                for (int j = i; j < g_muted_count - 1; j++) {
                    memcpy(g_muted[j], g_muted[j + 1], MAX_NAME_LEN);
                }
                g_muted_count--;
                removed++;
            } else {
                i++;
            }
        }
        if (removed > 0) {
            SaveMutedListLocked();
            WriteLog("User unmuted via command: '%s' (%d removed)", arg, removed);
        }
        LeaveCriticalSection(&g_cs);

        char feedback[128];
        if (removed > 0) {
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Unmuted: %s", arg);
        } else {
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Not found in mute list: %s", arg);
        }
        ShowLocalChatFeedback(feedback);
        return 1;
    }

    if (_stricmp(utf8_cmd, "/mutelist") == 0 || _stricmp(utf8_cmd, "/muted") == 0) {
        char feedback[256];
        EnterCriticalSection(&g_cs);
        LoadMutedListLocked(0);
        if (g_muted_count == 0) {
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Mute list is empty.");
        } else {
            _snprintf_s(feedback, sizeof(feedback), _TRUNCATE, "Muted (%d): ", g_muted_count);
            for (int i = 0; i < g_muted_count; i++) {
                if (i > 0) strncat_s(feedback, sizeof(feedback), ", ", _TRUNCATE);
                strncat_s(feedback, sizeof(feedback), g_muted[i], _TRUNCATE);
                if (strlen(feedback) > 200) {
                    strncat_s(feedback, sizeof(feedback), "...", _TRUNCATE);
                    break;
                }
            }
        }
        LeaveCriticalSection(&g_cs);
        ShowLocalChatFeedback(feedback);
        return 1;
    }

    return 0;
}

static void __fastcall Hooked_OnChatInputSubmit(
    void* this_ptr,
    void* edx_unused,
    int channel_id,
    EastlString16* wtext
) {
    (void)edx_unused;
    if (wtext != NULL) {
        char utf8_buf[256];
        utf8_buf[0] = '\0';
        __try {
            const wchar_t* wbegin = wtext->mpBegin;
            const wchar_t* wend   = wtext->mpEnd;
            if (wbegin && wend >= wbegin) {
                int wlen = (int)(wend - wbegin);
                if (wlen > 0 && wlen < 512) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, wbegin, wlen, utf8_buf, sizeof(utf8_buf) - 1, NULL, NULL);
                    if (n > 0) utf8_buf[n] = '\0';
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            utf8_buf[0] = '\0';
        }

        if (utf8_buf[0] == '/' && HandleSlashCommand(utf8_buf)) {
            return; /* Swallow command so it is NOT sent to the server */
        }
    }

    g_orig_OnChatInputSubmit(this_ptr, NULL, channel_id, wtext);
}

/* =========================================================================
 * Non-destructive Instance Vtable Hook Monitor Thread
 * ========================================================================= */
static void PrepareCloneVtables(void) {
    /* 1. UI Event vtable (0x01440804, RTTI at -4 = 0x01440800) */
    memcpy(g_clone_vtable_ui_event, (const void*)(VTABLE_UI_EVENT_EXPECTED - 4), 6 * sizeof(uint32_t));
    g_clone_vtable_ui_event[1] = (uint32_t)&Hooked_OnUiEvent;

    /* 2. HUD Event vtable (0x0142B794, RTTI at -4 = 0x0142B790) */
    memcpy(g_clone_vtable_hud_event, (const void*)(VTABLE_INGAME_HUD_SUB - 4), 6 * sizeof(uint32_t));
    g_clone_vtable_hud_event[1] = (uint32_t)&Hooked_OnHudEvent;

    /* 3. Chat Input Submit vtable (0x0142A3D0, RTTI at -4 = 0x0142A3CC) */
    memcpy(g_clone_vtable_chat_input, (const void*)(VTABLE_CHAT_INPUT_EXPECTED - 4), 6 * sizeof(uint32_t));
    g_clone_vtable_chat_input[1] = (uint32_t)&Hooked_OnChatInputSubmit;
}

static int IsExpectedBc2Client(void) {
    __try {
        if (*(uint32_t*)(VTABLE_UI_EVENT_EXPECTED + 4) != FN_ON_UI_EVENT_EXPECTED) return 0;
        if (*(uint32_t*)(VTABLE_CHAT_INPUT_EXPECTED + 0) != FN_ON_CHAT_INPUT_EXPECTED) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static DWORD WINAPI HookMonitorThread(LPVOID param) {
    (void)param;
    Sleep(500);

    if (!IsExpectedBc2Client()) {
        WriteLog("Process does not match expected BFBC2Game.exe memory signatures; staying inactive.");
        return 0;
    }

    PrepareCloneVtables();
    WriteLog("BC2 ChatFilter initialized. Monitoring UI singleton at 0x%08X...", ADDR_UI_SINGLETON_PTR);

    int logged_ui_hook = 0;
    int logged_input_hook = 0;

    while (g_running) {
        __try {
            uintptr_t ui_root = *(uintptr_t*)ADDR_UI_SINGLETON_PTR;
            if (ui_root && *(uint32_t*)ui_root == VTABLE_UI_ROOT_EXPECTED) {
                /* 1. Hook UI Event subobject at ui_root + 0x82AF8 */
                uint32_t* p_ui_event_vt = (uint32_t*)(ui_root + OFFSET_UI_EVENT_SUBOBJECT);
                if (*p_ui_event_vt == VTABLE_UI_EVENT_EXPECTED) {
                    InterlockedExchange((volatile LONG*)p_ui_event_vt, (LONG)(uintptr_t)&g_clone_vtable_ui_event[1]);
                    if (!logged_ui_hook) {
                        WriteLog("Attached UI chat filter hook on subobject 0x%08X", (unsigned int)(ui_root + OFFSET_UI_EVENT_SUBOBJECT));
                        logged_ui_hook = 1;
                    }
                }

                /* 2. Hook In-Game HUD subobject if active at [ui_root + 0x28] + 0x2C */
                uintptr_t hud_obj = *(uintptr_t*)(ui_root + 0x82B20);
                if (hud_obj) {
                    uint32_t* p_hud_sub_vt = (uint32_t*)(hud_obj + 0x2C);
                    if (*p_hud_sub_vt == VTABLE_INGAME_HUD_SUB) {
                        InterlockedExchange((volatile LONG*)p_hud_sub_vt, (LONG)(uintptr_t)&g_clone_vtable_hud_event[1]);
                    }
                }

                /* 3. Hook Chat Input handler at [ui_root + 0x82D0C] channels 3, 5, 6, 7 */
                uintptr_t tb_mgr = *(uintptr_t*)(ui_root + OFFSET_UI_TEXTBOX_MGR_PTR);
                if (tb_mgr) {
                    const int channels[4] = {3, 5, 6, 7};
                    for (int i = 0; i < 4; i++) {
                        uintptr_t handler = *(uintptr_t*)(tb_mgr + channels[i] * 4);
                        if (handler) {
                            uint32_t* p_vt = (uint32_t*)handler;
                            if (*p_vt == VTABLE_CHAT_INPUT_EXPECTED) {
                                InterlockedExchange((volatile LONG*)p_vt, (LONG)(uintptr_t)&g_clone_vtable_chat_input[1]);
                                if (!logged_input_hook) {
                                    WriteLog("Attached in-game /mute command hook on chat input handler 0x%08X", (unsigned int)handler);
                                    logged_input_hook = 1;
                                }
                            }
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* Ignore transient UI teardown states */
        }

        Sleep(250);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_cs);
        LoadRealMsimg32();

        GetModuleFileNameA(hModule, g_dll_dir, MAX_PATH);
        char* last_slash = strrchr(g_dll_dir, '\\');
        if (last_slash) *(last_slash + 1) = '\0';

        _snprintf_s(g_mute_file, sizeof(g_mute_file), _TRUNCATE, "%smuted_players.txt", g_dll_dir);
        _snprintf_s(g_log_file, sizeof(g_log_file), _TRUNCATE, "%schat_filter.log", g_dll_dir);

        EnterCriticalSection(&g_cs);
        LoadMutedListLocked(1);
        LeaveCriticalSection(&g_cs);

        g_worker_thread = CreateThread(NULL, 0, HookMonitorThread, NULL, 0, NULL);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_running = 0;
    }
    return TRUE;
}
