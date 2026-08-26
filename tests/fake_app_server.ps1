$ErrorActionPreference = 'Stop'
while ($null -ne ($line = [Console]::In.ReadLine())) {
    try { $request = $line | ConvertFrom-Json } catch { continue }
    if ($request.method -eq 'initialize') {
        @{ id = $request.id; result = @{ userAgent = 'fake/1.0'; codexHome = "$env:USERPROFILE\.codex"; platformFamily = 'windows'; platformOs = 'Windows' } } | ConvertTo-Json -Compress -Depth 10
    } elseif ($request.method -eq 'account/read') {
        @{ id = $request.id; result = @{ account = @{ type = 'chatgpt'; planType = 'pro' }; requiresOpenaiAuth = $true } } | ConvertTo-Json -Compress -Depth 10
    } elseif ($request.method -eq 'account/rateLimits/read') {
        @{ id = $request.id; result = @{ rateLimits = @{ limitId = 'codex'; planType = 'pro'; primary = @{ usedPercent = 32; windowDurationMins = 300; resetsAt = [DateTimeOffset]::UtcNow.AddHours(3).ToUnixTimeSeconds() }; secondary = @{ usedPercent = 61; windowDurationMins = 10080; resetsAt = [DateTimeOffset]::UtcNow.AddDays(4).ToUnixTimeSeconds() } } } } | ConvertTo-Json -Compress -Depth 10
    } elseif ($request.method -eq 'account/usage/read') {
        @{ id = $request.id; result = @{ summary = @{ lifetimeTokens = 2600000000; peakDailyTokens = 180000000 }; dailyUsageBuckets = @(@{ startDate = '2026-08-24'; tokens = 42000000 }, @{ startDate = '2026-08-25'; tokens = 65000000 }, @{ startDate = '2026-08-26'; tokens = 31000000 }) } } | ConvertTo-Json -Compress -Depth 10
    } elseif ($null -ne $request.id) {
        @{ id = $request.id; error = @{ code = -32601; message = 'Method not found' } } | ConvertTo-Json -Compress -Depth 10
    }
}

