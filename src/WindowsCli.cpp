// Windows shells prefer oc.com over oc.exe. This console entry point waits
// for CLI output, but yields immediately for ordinary GUI launches.
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {
std::wstring quote(const std::wstring &argument)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
}

int main()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        return 1;
    std::wstring executable(path.data(), length);
    executable = executable.substr(0, executable.find_last_of(L"\\/") + 1) + L"oc.exe";
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    bool wait = false;
    std::wstring command = quote(executable);
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        wait = wait || argument == L"--help" || argument == L"-h"
            || argument == L"--version" || argument == L"-V"
            || argument == L"--wait" || argument == L"-w";
        command += L" " + quote(argument);
    }
    LocalFree(argv);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &startup, &process)) {
        const std::string error = "oc: could not start oc.exe (Windows error "
            + std::to_string(GetLastError()) + ").\n";
        DWORD written;
        WriteFile(startup.hStdError, error.data(), static_cast<DWORD>(error.size()), &written, nullptr);
        return 1;
    }
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
