#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

static void log_str(const char *s) {
    HANDLE h = CreateFileA("C:\\Riot Games\\League of Legends\\Logs\\shim.log",
                           FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        DWORD len = 0;
        while (s[len]) len++;
        WriteFile(h, s, len, &written, NULL);
        CloseHandle(h);
    }
}

static void log_num(int n) {
    char buf[32];
    int idx = 0;
    if (n == 0) {
        buf[idx++] = '0';
    } else {
        char tmp[32];
        int t = 0;
        unsigned int u = (unsigned int)n;
        while (u > 0) {
            tmp[t++] = '0' + (u % 10);
            u /= 10;
        }
        while (t > 0) {
            buf[idx++] = tmp[--t];
        }
    }
    buf[idx] = '\0';
    log_str(buf);
}

static int parse_port(const WCHAR *cmd) {
    const WCHAR *p = cmd;
    const WCHAR *key = L"--app-port=";
    int keylen = 11;
    while (*p) {
        int match = 1;
        for (int i = 0; i < keylen; i++) {
            if (p[i] != key[i]) { match = 0; break; }
        }
        if (match) {
            p += keylen;
            int port = 0;
            while (*p >= L'0' && *p <= L'9') {
                port = port * 10 + (*p - L'0');
                p++;
            }
            return port;
        }
        p++;
    }
    return 0;
}

void mainCRTStartup(void) {
    log_str("[SHIM] LeagueClientUxShim started!\r\n");

    WCHAR *cmd = GetCommandLineW();
    int port = parse_port(cmd);

    log_str("[SHIM] Detected target port: ");
    log_num(port);
    log_str("\r\n");

    if (port > 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        log_str("[SHIM] Waiting for Foundation port to open...\r\n");
        int connected = 0;
        for (int attempt = 1; attempt <= 300; attempt++) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                struct sockaddr_in addr;
                addr.sin_family = AF_INET;
                addr.sin_port = htons((unsigned short)port);
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");

                if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    closesocket(s);
                    connected = 1;
                    log_str("[SHIM] Port connected at attempt ");
                    log_num(attempt);
                    log_str("!\r\n");
                    break;
                }
                closesocket(s);
            }
            Sleep(1000);
        }
        WSACleanup();

        if (!connected) {
            log_str("[SHIM] ERROR: Timed out waiting for Foundation port after 300s!\r\n");
            ExitProcess(1);
        }
    } else {
        log_str("[SHIM] WARNING: No --app-port found in command line, proceeding directly.\r\n");
    }

    /* Small delay for listener stability */
    Sleep(500);

    /* Construct real UX command line */
    /* Replace shim binary name with 'LeagueClientUx' in the command line */
    WCHAR real_cmd[4096];
    int r_idx = 0;
    const WCHAR *src = cmd;
    while (*src && r_idx < 4000) {
        const WCHAR *p_rel = L"../LeagueUxS01";
        const WCHAR *p_name = L"LeagueUxS01";
        const WCHAR *p1 = L"LeagueUxShim01";
        const WCHAR *p2 = L"LeagueClientUxShim";
        int m_rel = 1, m_name = 1, match1 = 1, match2 = 1;
        for (int i = 0; p_rel[i]; i++) {
            if (src[i] != p_rel[i]) { m_rel = 0; break; }
        }
        for (int i = 0; p_name[i]; i++) {
            if (src[i] != p_name[i]) { m_name = 0; break; }
        }
        for (int i = 0; p1[i]; i++) {
            if (src[i] != p1[i]) { match1 = 0; break; }
        }
        for (int i = 0; p2[i]; i++) {
            if (src[i] != p2[i]) { match2 = 0; break; }
        }
        if (m_rel) {
            const WCHAR *replacement = L"LeagueClientUx";
            for (int i = 0; replacement[i]; i++) {
                real_cmd[r_idx++] = replacement[i];
            }
            src += 14; /* length of ../LeagueUxS01 */
        } else if (m_name) {
            const WCHAR *replacement = L"LeagueClientUx";
            for (int i = 0; replacement[i]; i++) {
                real_cmd[r_idx++] = replacement[i];
            }
            src += 11; /* length of LeagueUxS01 */
        } else if (match1) {
            const WCHAR *replacement = L"LeagueClientUx";
            for (int i = 0; replacement[i]; i++) {
                real_cmd[r_idx++] = replacement[i];
            }
            src += 14; /* length of LeagueUxShim01 */
        } else if (match2) {
            const WCHAR *replacement = L"LeagueClientUx";
            for (int i = 0; replacement[i]; i++) {
                real_cmd[r_idx++] = replacement[i];
            }
            src += 18; /* length of LeagueClientUxShim */
        } else {
            real_cmd[r_idx++] = *src++;
        }
    }

    /* Append extra CEF stability flags to avoid renderer GPU/compositor crash */
    const WCHAR *extra_flags = L" --disable-gpu --disable-gpu-compositing";
    for (int i = 0; extra_flags[i] && r_idx < 4000; i++) {
        real_cmd[r_idx++] = extra_flags[i];
    }
    real_cmd[r_idx] = L'\0';

    /* Set environment overrides to prevent GStreamer crashes under Wayland/Wine */
    SetEnvironmentVariableW(L"WINEDLLOVERRIDES", L"winegstreamer=d");
    SetEnvironmentVariableW(L"GST_GL_WINDOW", L"x11");

    log_str("[SHIM] Appended flags: --disable-gpu --disable-gpu-compositing\r\n");
    log_str("[SHIM] Launching real LeagueClientUx.exe...\r\n");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    for (int i = 0; i < (int)sizeof(si); i++) ((char*)&si)[i] = 0;
    for (int i = 0; i < (int)sizeof(pi); i++) ((char*)&pi)[i] = 0;
    si.cb = sizeof(si);

    BOOL created = CreateProcessW(
        L"C:\\Riot Games\\League of Legends\\LeagueClientUx.exe",
        real_cmd,
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!created) {
        log_str("[SHIM] ERROR: Failed to launch LeagueClientUx.exe! Error code: ");
        log_num((int)GetLastError());
        log_str("\r\n");
        ExitProcess(GetLastError());
    }

    log_str("[SHIM] LeagueClientUx.exe launched successfully! Waiting for process...\r\n");
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    log_str("[SHIM] LeagueClientUx.exe exited with code: ");
    log_num((int)exit_code);
    log_str(". Forwarding exit.\r\n");

    ExitProcess(exit_code);
}
