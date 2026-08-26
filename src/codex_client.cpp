#include "codex_client.hpp"

#include "settings.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace cqt {
namespace {

std::wstring quote_argument(std::wstring value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'"');
            slashes = 0;
        } else {
            output.append(slashes, L'\\');
            slashes = 0;
            output.push_back(character);
        }
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

bool is_file(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::optional<std::wstring> find_on_path() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = SearchPathW(nullptr, L"codex.exe", nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
    if (length == 0 || length >= path.size()) return std::nullopt;
    return std::wstring(path.data(), length);
}

std::optional<std::wstring> find_desktop_cache() {
    std::array<wchar_t, 32768> local_app_data{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(),
                                                  static_cast<DWORD>(local_app_data.size()));
    if (length == 0 || length >= local_app_data.size()) return std::nullopt;

    std::wstring base(local_app_data.data(), length);
    base += L"\\OpenAI\\Codex\\bin";
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((base + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return std::nullopt;

    std::optional<std::wstring> newest;
    FILETIME newest_time{};
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
        std::wstring candidate = base + L"\\" + entry.cFileName + L"\\codex.exe";
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &attributes) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (!newest || CompareFileTime(&attributes.ftLastWriteTime, &newest_time) > 0) {
            newest = std::move(candidate);
            newest_time = attributes.ftLastWriteTime;
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return newest;
}

std::wstring resolve_codex_executable(const std::wstring& configured_executable) {
    if (!configured_executable.empty() && is_file(configured_executable)) return configured_executable;
    if (const auto desktop_executable = find_desktop_cache()) return *desktop_executable;
    if (const auto path_executable = find_on_path()) return *path_executable;
    return configured_executable.empty() ? L"codex.exe" : configured_executable;
}

json::Value initialize_params() {
    json::Value::Object client_info;
    client_info["name"] = "codex_quota_tray";
    client_info["title"] = "CodexQuotaTray";
    client_info["version"] = "1.0.0";

    json::Value::Object capabilities;
    json::Value::Array opt_out;
    opt_out.emplace_back("thread/started");
    opt_out.emplace_back("item/agentMessage/delta");
    capabilities["optOutNotificationMethods"] = std::move(opt_out);

    json::Value::Object params;
    params["clientInfo"] = std::move(client_info);
    params["capabilities"] = std::move(capabilities);
    return params;
}

} // namespace

CodexClient::~CodexClient() {
    stop();
}

bool CodexClient::start(const std::wstring& configured_executable, std::string& error) {
    stop();
    stopping_.store(false);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    if (!CreatePipe(&stdout_read_, &child_stdout_write, &security, 0) ||
        !SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&child_stdin_read, &stdin_write_, &security, 0) ||
        !SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
        error = "Unable to create app-server pipes (Windows error " + std::to_string(GetLastError()) + ")";
        if (child_stdin_read) CloseHandle(child_stdin_read);
        if (child_stdout_write) CloseHandle(child_stdout_write);
        stop();
        return false;
    }

    HANDLE null_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = null_error == INVALID_HANDLE_VALUE ? child_stdout_write : null_error;

    const std::wstring executable = resolve_codex_executable(configured_executable);
    std::wstring command = quote_argument(executable) + L" app-server --stdio";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    PROCESS_INFORMATION process_info{};
    const wchar_t* application_name = is_file(executable) ? executable.c_str() : nullptr;
    const BOOL created = CreateProcessW(application_name, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                                        &startup, &process_info);
    const DWORD create_error = GetLastError();
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    if (null_error != INVALID_HANDLE_VALUE) CloseHandle(null_error);
    if (!created) {
        error = "Unable to start Codex app-server (Windows error " + std::to_string(create_error) + ")";
        stop();
        return false;
    }

    CloseHandle(process_info.hThread);
    process_ = process_info.hProcess;
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job_, process_);
    }

    reader_ = std::thread(&CodexClient::reader_loop, this);
    const auto initialized = request("initialize", initialize_params(), std::chrono::seconds(15));
    if (!initialized.ok) {
        error = initialized.error.empty() ? "Codex app-server initialization failed" : initialized.error;
        stop();
        return false;
    }
    if (!notify("initialized")) {
        error = "Unable to acknowledge Codex app-server initialization";
        stop();
        return false;
    }
    return true;
}

void CodexClient::stop() {
    stopping_.store(true);
    HANDLE local_stdin = nullptr;
    HANDLE local_stdout = nullptr;
    HANDLE local_process = nullptr;
    HANDLE local_job = nullptr;
    {
        std::lock_guard lock(state_mutex_);
        local_stdin = std::exchange(stdin_write_, nullptr);
        local_stdout = std::exchange(stdout_read_, nullptr);
        local_process = std::exchange(process_, nullptr);
        local_job = std::exchange(job_, nullptr);
    }
    if (local_stdin) CloseHandle(local_stdin);
    if (local_stdout) CloseHandle(local_stdout);
    if (local_process) {
        if (WaitForSingleObject(local_process, 250) == WAIT_TIMEOUT) TerminateProcess(local_process, 0);
        CloseHandle(local_process);
    }
    if (local_job) CloseHandle(local_job);
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    fail_all("Codex app-server stopped");
}

bool CodexClient::running() const {
    std::lock_guard lock(state_mutex_);
    if (!process_) return false;
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

RpcResult CodexClient::request(std::string method, std::optional<json::Value> params, std::chrono::milliseconds timeout) {
    if (!running()) return {false, {}, "Codex app-server is not running"};
    const std::int64_t id = next_id_.fetch_add(1);
    auto pending = std::make_shared<PendingRequest>();
    {
        std::lock_guard lock(state_mutex_);
        pending_[id] = pending;
    }

    json::Value::Object message;
    message["id"] = id;
    message["method"] = std::move(method);
    if (params) message["params"] = std::move(*params);
    if (!write_message(message)) {
        std::lock_guard lock(state_mutex_);
        pending_.erase(id);
        return {false, {}, "Unable to write to Codex app-server"};
    }

    std::unique_lock lock(pending->mutex);
    if (!pending->condition.wait_for(lock, timeout, [&] { return pending->completed; })) {
        lock.unlock();
        std::lock_guard state_lock(state_mutex_);
        pending_.erase(id);
        return {false, {}, "Codex app-server request timed out"};
    }
    return std::move(pending->result);
}

bool CodexClient::notify(std::string method, std::optional<json::Value> params) {
    json::Value::Object message;
    message["method"] = std::move(method);
    if (params) message["params"] = std::move(*params);
    return write_message(message);
}

void CodexClient::set_notification_callback(NotificationCallback callback) {
    std::lock_guard lock(state_mutex_);
    notification_callback_ = std::move(callback);
}

bool CodexClient::write_message(const json::Value& message) {
    std::string data = json::stringify(message);
    data.push_back('\n');
    std::lock_guard write_lock(write_mutex_);
    HANDLE output = nullptr;
    {
        std::lock_guard state_lock(state_mutex_);
        output = stdin_write_;
    }
    if (!output) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(output, data.data() + offset, chunk, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

void CodexClient::reader_loop() {
    std::array<char, 8192> buffer{};
    std::string pending_text;
    while (!stopping_.load()) {
        HANDLE input = nullptr;
        {
            std::lock_guard lock(state_mutex_);
            input = stdout_read_;
        }
        if (!input) break;
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
        pending_text.append(buffer.data(), read);
        std::size_t newline = 0;
        while ((newline = pending_text.find('\n')) != std::string::npos) {
            std::string line = pending_text.substr(0, newline);
            pending_text.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) dispatch_line(line);
        }
    }
    if (!stopping_.load()) fail_all("Codex app-server connection closed");
}

void CodexClient::dispatch_line(std::string_view line) {
    const auto parsed = json::parse(line);
    if (!parsed || !parsed.value.is_object()) return;
    const auto* id_value = parsed.value.find("id");
    if (id_value && id_value->is_number()) {
        const std::int64_t id = id_value->as_int64(-1);
        std::shared_ptr<PendingRequest> pending;
        {
            std::lock_guard lock(state_mutex_);
            const auto iterator = pending_.find(id);
            if (iterator == pending_.end()) return;
            pending = iterator->second;
            pending_.erase(iterator);
        }
        RpcResult result;
        if (const auto* error = parsed.value.find("error"); error && error->is_object()) {
            result.ok = false;
            if (const auto* message = error->find("message")) result.error = message->string_or("Codex app-server error");
            if (result.error.empty()) result.error = "Codex app-server error";
        } else if (const auto* value = parsed.value.find("result")) {
            result.ok = true;
            result.value = *value;
        } else {
            result.error = "Malformed Codex app-server response";
        }
        {
            std::lock_guard lock(pending->mutex);
            pending->completed = true;
            pending->result = std::move(result);
        }
        pending->condition.notify_all();
        return;
    }

    const auto* method = parsed.value.find("method");
    if (!method || !method->is_string()) return;
    NotificationCallback callback;
    {
        std::lock_guard lock(state_mutex_);
        callback = notification_callback_;
    }
    if (callback) callback(method->as_string());
}

void CodexClient::fail_all(std::string error) {
    std::map<std::int64_t, std::shared_ptr<PendingRequest>> pending;
    {
        std::lock_guard lock(state_mutex_);
        pending.swap(pending_);
    }
    for (auto& [id, request] : pending) {
        (void)id;
        {
            std::lock_guard lock(request->mutex);
            request->completed = true;
            request->result = {false, {}, error};
        }
        request->condition.notify_all();
    }
}

} // namespace cqt
