// Compiled into every game module on Windows (cmake/ohao_games.cmake), never into the engine: the module's calls
// into the engine are imports from "ohao_engine.exe", delay-loaded (/DELAYLOAD), and the hook below answers that name
// with the executable that loaded the module, whichever it is (ohao, ohao_game). No file of that name exists, so
// nothing else can answer it. game/loader.hpp calls ohao_game_bind right after the load: every import bound, or the
// first the executable does not export named.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <delayimp.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr char kHost[] = "ohao_engine.exe";
char g_missing[512] = {};

FARPROC WINAPI notify(unsigned event, PDelayLoadInfo info) {
    if (event == dliNotePreLoadLibrary && _stricmp(info->szDll, kHost) == 0)
        return reinterpret_cast<FARPROC>(GetModuleHandleW(nullptr));
    return nullptr;
}

FARPROC WINAPI failure(unsigned event, PDelayLoadInfo info) {
    if (event == dliFailGetProc) {
        if (info->dlp.fImportByName)
            std::snprintf(g_missing, sizeof g_missing,
                          "the executable does not export %s (tools/gen_game_imports.py lists what the games import: "
                          "rebuild the host)", info->dlp.szProcName);
        else
            std::snprintf(g_missing, sizeof g_missing, "the executable does not export ordinal %lu",
                          static_cast<unsigned long>(info->dlp.dwOrdinal));
    } else if (event == dliFailLoadLib) {
        std::snprintf(g_missing, sizeof g_missing, "%s was not answered", info->szDll);
    }
    return nullptr;
}

// A module that imports nothing from the engine has no descriptor for it: nothing to bind.
bool bindAll() {
    HRESULT hr = S_OK;
    __try {
        hr = __HrLoadAllImportsForDll(kHost);
    } __except (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) ||
                        GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND)
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = notify;
extern "C" const PfnDliHook __pfnDliFailureHook2 = failure;

// Null when every import is bound, else why not.
extern "C" __declspec(dllexport) const char* ohao_game_bind() {
    if (bindAll()) return nullptr;
    if (!g_missing[0]) std::strcpy(g_missing, "binding the engine's imports failed");
    return g_missing;
}
