#include "app.hpp"
#include "ui.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

namespace {

bool has_argument(std::wstring_view value) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool found = false;
    for (int i = 1; i < count; ++i) {
        if (value == arguments[i]) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\CodexQuotaTray.Singleton.v1");
    if (!singleton) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"CodexQuotaTray.MainWindow", nullptr)) {
            PostMessageW(existing, cqt::AppController::kShowMessage, 0, 0);
        }
        CloseHandle(singleton);
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }

    cqt::AppController app(instance);
    std::wstring error;
    if (!app.initialize(has_argument(L"--background"), error)) {
        MessageBoxW(nullptr, error.c_str(), L"CodexQuotaTray", MB_OK | MB_ICONERROR);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    const int exit_code = app.run();
    app.shutdown();
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    if (SUCCEEDED(com)) CoUninitialize();
    return exit_code;
}
