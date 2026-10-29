// Static MSVC CRT pieces referenced by nvsdk_ngx_d.lib that neither mingw-w64 nor
// vcruntime140.dll export. Linked into every module that links the NGX lib.
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" {
// marks float use for the MSVC linker; only the symbol matters
int _fltused = 0;

// MSVC default value; mingw has no __security_init_cookie to randomize it
uintptr_t __security_cookie = 0x00002B992DDFA232ull;

// read inline by MSVC codegen through _tls_index, so it must be native TLS
thread_local int _Init_thread_epoch = INT_MIN;

EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD* record, void* frame, CONTEXT* context,
                                         DISPATCHER_CONTEXT* dispatch);

// GS cookie validation during unwind is skipped; epilogue checks still run
EXCEPTION_DISPOSITION __GSHandlerCheck(EXCEPTION_RECORD*, void*, CONTEXT*, DISPATCHER_CONTEXT*)
{
    return ExceptionContinueSearch;
}

// HandlerData starts with the FuncInfo RVA in both handlers, so forwarding is exact
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(EXCEPTION_RECORD* record, void* frame, CONTEXT* context,
                                          DISPATCHER_CONTEXT* dispatch)
{
    return __CxxFrameHandler3(record, frame, context, dispatch);
}
}

// asm: called in epilogues with the return value live in rax/xmm0
asm(R"(
    .text
    .globl __security_check_cookie
__security_check_cookie:
    cmpq __security_cookie(%rip), %rcx
    jne 1f
    ret
1:
    movl $2, %ecx # FAST_FAIL_STACK_COOKIE_CHECK_FAILURE
    int $0x29
)");

// MSVC thread-safe statics protocol (vcruntime thread_safe_statics.cpp)
namespace
{
    constexpr int kUninitialized = 0;
    constexpr int kBeingInitialized = -1;

    std::mutex initMutex;
    std::condition_variable initDone;
    int globalEpoch = INT_MIN;
} // namespace

extern "C" void _Init_thread_header(int* once)
{
    std::unique_lock lock(initMutex);
    initDone.wait(lock, [once] { return *once != kBeingInitialized; });
    // uninitialized also after another thread's initializer threw
    if (*once == kUninitialized) {
        *once = kBeingInitialized;
        return;
    }
    _Init_thread_epoch = globalEpoch;
}

extern "C" void _Init_thread_footer(int* once)
{
    {
        std::lock_guard lock(initMutex);
        *once = ++globalEpoch;
        _Init_thread_epoch = globalEpoch;
    }
    initDone.notify_all();
}

// MSVC-mangled global operator new/delete; aborting keeps Itanium exceptions out of MSVC frames
void* msvcOperatorNew(size_t size) asm("??2@YAPEAX_K@Z");
void msvcOperatorDelete(void* pointer) asm("??3@YAXPEAX@Z");

void* msvcOperatorNew(size_t size)
{
    if (void* pointer = ::operator new(size, std::nothrow)) {
        return pointer;
    }
    std::abort();
}

void msvcOperatorDelete(void* pointer) { ::operator delete(pointer); }

// msvcp140 throw helpers, referenced through dllimport slots
namespace
{
    [[noreturn]] void msvcXBadAlloc() { std::abort(); }

    [[noreturn]] void msvcXLengthError(const char*) { std::abort(); }
} // namespace

extern "C" void (*const msvcXBadAllocImport)() asm("__imp_?_Xbad_alloc@std@@YAXXZ");
extern "C" void (*const msvcXLengthErrorImport)(const char*) asm("__imp_?_Xlength_error@std@@YAXPEBD@Z");
void (*const msvcXBadAllocImport)() = msvcXBadAlloc;
void (*const msvcXLengthErrorImport)(const char*) = msvcXLengthError;
