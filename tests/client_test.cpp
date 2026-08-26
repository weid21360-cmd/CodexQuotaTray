#include "codex_client.hpp"
#include "usage_parser.hpp"

#include <Windows.h>

#include <iostream>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Expected fake app-server path\n";
        return 2;
    }
    cqt::CodexClient client;
    std::string error;
    if (!client.start(std::filesystem::path(argv[1]).wstring(), error)) {
        std::cerr << "start failed: " << error << '\n';
        return 1;
    }
    cqt::UsageSnapshot snapshot;
    cqt::json::Value::Object account_params;
    account_params["refreshToken"] = false;
    const auto account = client.request("account/read", account_params);
    const auto quota = client.request("account/rateLimits/read");
    const auto usage = client.request("account/usage/read");
    bool ok = account.ok && quota.ok && usage.ok;
    ok = ok && cqt::parse_account(account.value, snapshot).ok;
    ok = ok && cqt::parse_rate_limits(quota.value, snapshot).ok;
    ok = ok && cqt::parse_account_usage(usage.value, snapshot).ok;
    ok = ok && snapshot.plan_type == "pro" && snapshot.quota_windows.size() == 2;
    ok = ok && snapshot.lifetime_tokens == 2'600'000'000LL;
    client.stop();
    if (!ok) {
        std::cerr << "JSON-RPC integration assertions failed\n";
        return 1;
    }
    std::cout << "CodexClient fake app-server integration passed\n";
    return 0;
}
