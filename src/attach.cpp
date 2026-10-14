#include "attach.h"
#include "log.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace attach {
namespace {

// Remote parameter block. Offsets are baked into kStub below.
#pragma pack(push, 1)
struct Block {
    void* getModuleHandleA;          // 0x000
    void* getProcAddress;            // 0x008
    char  jvmLib[16];                // 0x010  "jvm.dll"
    char  func[32];                  // 0x020  "JVM_EnqueueOperation"
    char  cmd[16];                   // 0x040
    char  arg0[0x410];               // 0x050
    char  arg1[0x410];               // 0x460
    char  arg2[0x410];               // 0x870
    char  pipe[0x100];               // 0xC80
};
#pragma pack(pop)
static_assert(offsetof(Block, jvmLib) == 0x10 && offsetof(Block, func) == 0x20 && offsetof(Block, cmd) == 0x40);
static_assert(offsetof(Block, arg0) == 0x50 && offsetof(Block, arg1) == 0x460 && offsetof(Block, arg2) == 0x870);
static_assert(offsetof(Block, pipe) == 0xC80);

// DWORD stub(Block* b):
//   h = b->getModuleHandleA(b->jvmLib)          ; 0x65 if jvm.dll missing
//   f = b->getProcAddress(h, b->func)           ; 0x66 if export missing
//   return f(b->cmd, b->arg0, b->arg1, b->arg2, b->pipe)
// (assembled with ml64, see re/stub.asm)
const unsigned char kStub[] = {
    0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x4B, 0x10, 0xFF, 0x13, 0x48, 0x85,
    0xC0, 0x74, 0x35, 0x48, 0x8B, 0xC8, 0x48, 0x8D, 0x53, 0x20, 0xFF, 0x53, 0x08, 0x48, 0x85, 0xC0,
    0x74, 0x2D, 0x48, 0x8D, 0x4B, 0x40, 0x48, 0x8D, 0x53, 0x50, 0x4C, 0x8D, 0x83, 0x60, 0x04, 0x00,
    0x00, 0x4C, 0x8D, 0x8B, 0x70, 0x08, 0x00, 0x00, 0x4C, 0x8D, 0x93, 0x80, 0x0C, 0x00, 0x00, 0x4C,
    0x89, 0x54, 0x24, 0x20, 0xFF, 0xD0, 0xEB, 0x0C, 0xB8, 0x65, 0x00, 0x00, 0x00, 0xEB, 0x05, 0xB8,
    0x66, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x30, 0x5B, 0xC3,
};

struct Remote {
    HANDLE proc;
    void*  p = nullptr;
    explicit Remote(HANDLE h) : proc(h) {}
    ~Remote() { if (p) VirtualFreeEx(proc, p, 0, MEM_RELEASE); }
};

bool Copy(char* dst, size_t cap, const std::string& s) {
    if (s.size() >= cap) return false;
    memcpy(dst, s.c_str(), s.size() + 1);
    return true;
}

} // namespace

bool LoadAgent(DWORD pid, const std::string& agentJar, const std::string& options, std::string& err) {
    // The JVM's attach listener rejects pipe names without this prefix.
    char pipeName[128];
    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\javatool%lu_%lu_%lu", GetCurrentProcessId(), pid, GetTickCount());

    Block b{};
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");   // same base in every process this boot
    b.getModuleHandleA = (void*)GetProcAddress(k32, "GetModuleHandleA");
    b.getProcAddress   = (void*)GetProcAddress(k32, "GetProcAddress");
    strcpy_s(b.jvmLib, "jvm.dll");
    strcpy_s(b.func, "JVM_EnqueueOperation");
    strcpy_s(b.cmd, "load");
    strcpy_s(b.arg0, "instrument");
    strcpy_s(b.arg1, "false");
    // The JVM caps each argument at 1024 characters.
    std::string a2 = agentJar + "=" + options;
    if (a2.size() > 1024 || !Copy(b.arg2, sizeof(b.arg2), a2)) { err = "agent path too long"; return false; }
    strcpy_s(b.pipe, pipeName);

    HANDLE pipe = CreateNamedPipeA(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 8192, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { err = "could not create reply pipe"; return false; }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!proc) { CloseHandle(pipe); err = "cannot open server process"; return false; }

    bool ok = false;
    {
        Remote code(proc), data(proc);
        code.p = VirtualAllocEx(proc, nullptr, sizeof(kStub), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        data.p = VirtualAllocEx(proc, nullptr, sizeof(Block), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD oldProt = 0;
        if (!code.p || !data.p ||
            !WriteProcessMemory(proc, code.p, kStub, sizeof(kStub), nullptr) ||
            !WriteProcessMemory(proc, data.p, &b, sizeof(b), nullptr) ||
            !VirtualProtectEx(proc, code.p, sizeof(kStub), PAGE_EXECUTE_READ, &oldProt)) {
            err = "cannot write attach request";
        } else {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            // pending until the JVM opens it (or already connected)
            bool connected = !ConnectNamedPipe(pipe, &ov) && GetLastError() == ERROR_PIPE_CONNECTED;

            HANDLE th = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)code.p, data.p, 0, nullptr);
            if (!th) {
                err = "cannot start attach thread";
            } else {
                DWORD rc = 1;
                if (WaitForSingleObject(th, 10000) != WAIT_OBJECT_0) {
                    err = "attach request timed out";
                    // leave the memory mapped: the thread may still be running in it
                    code.p = data.p = nullptr;
                } else {
                    GetExitCodeThread(th, &rc);
                    if (rc == 0x65)      err = "jvm.dll not loaded in server";
                    else if (rc == 0x66) err = "attach entry point missing";
                    else if (rc != 0)    err = "JVM refused attach (code " + std::to_string(rc) + ")";
                }
                CloseHandle(th);

                if (err.empty()) {
                    // Reply: "<status>\n<agent return code>\n[message]"; loading the agent can take a moment.
                    std::string reply;
                    if (connected || WaitForSingleObject(ov.hEvent, 20000) == WAIT_OBJECT_0) {
                        char buf[1024];
                        for (;;) {
                            OVERLAPPED ro{}; ro.hEvent = ov.hEvent; ResetEvent(ov.hEvent);
                            DWORD got = 0;
                            if (!ReadFile(pipe, buf, sizeof(buf), &got, &ro)) {
                                if (GetLastError() != ERROR_IO_PENDING) break;
                                if (WaitForSingleObject(ro.hEvent, 20000) != WAIT_OBJECT_0) { CancelIo(pipe); break; }
                                if (!GetOverlappedResult(pipe, &ro, &got, FALSE) || got == 0) break;
                            }
                            if (got == 0) break;
                            reply.append(buf, got);
                        }
                    }
                    for (auto& c : reply) if (c == '\n' || c == '\r') c = ' ';
                    log_::Info("attach reply: '%s'", reply.c_str());
                    // JDK 21+: "0 return code: 0"; older: "0 0"
                    int status = -1, agentRc = -1;
                    sscanf_s(reply.c_str(), "%d", &status);
                    size_t rcAt = reply.find("return code:");
                    if (rcAt != std::string::npos) sscanf_s(reply.c_str() + rcAt + 12, "%d", &agentRc);
                    else sscanf_s(reply.c_str(), "%*d %d", &agentRc);
                    if (status == 0 && agentRc == 0)
                        ok = true;
                    else
                        err = reply.empty() ? "no reply from JVM" : "agent load failed: " + reply;
                }
            }
            CloseHandle(ov.hEvent);
        }
    }
    CloseHandle(proc);
    CloseHandle(pipe);
    return ok;
}

} // namespace attach
