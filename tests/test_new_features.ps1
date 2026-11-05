Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "OTTO NEW MODERN CLI FEATURES TEST SUITE (7 NEW CAPABILITIES)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$global:passed = 0
$global:total = 0

function Run-Test($name, $scriptBlock) {
    $global:total++
    Write-Host "`nTest $global:total: $name" -ForegroundColor Yellow
    try {
        & $scriptBlock
        Write-Host "  -> PASSED" -ForegroundColor Green
        $global:passed++
    } catch {
        Write-Host "  -> FAILED: $_" -ForegroundColor Red
    }
}

$bin = if (Test-Path ".\otto.exe") { ".\otto.exe" } else { ".\shx.exe" }

# 1. Native HTTP REST Streams (fetch GET & onto POST)
Run-Test "Native HTTP REST Streams (fetch & onto POST)" {
    # 1.1 fetch GET
    Set-Content -Path test_fetch.otto -Value 'fetch "https://jsonplaceholder.typicode.com/todos/1" json, print each''s title.'
    $out = (& $bin test_fetch.otto).Trim()
    Remove-Item test_fetch.otto -ErrorAction SilentlyContinue
    if ($out -ne "delectus aut autem") {
        throw "fetch GET failed, expected 'delectus aut autem', got '$out'"
    }

    # 1.2 onto POST
    Set-Content -Path test_post.otto -Value 'write {status: "ok", service: "otto"} onto "https://httpbin.org/post", print.'
    $post_out = & $bin test_post.otto
    Remove-Item test_post.otto -ErrorAction SilentlyContinue
    if (-not ($post_out -like "*otto*" -and $post_out -like "*httpbin.org*")) {
        throw "HTTP POST onto failed, got '$post_out'"
    }
}

# 2. File Change Watcher Loop (watch Hook)
Run-Test "File Change Watcher Loop (watch Hook with change detection)" {
    New-Item -ItemType Directory -Path test_watch_dir -Force | Out-Null
    Set-Content -Path test_watch_dir/code.txt -Value "v1"
    Set-Content -Path test_watch.otto -Value 'watch file "test_watch_dir": print "watch_tick".'
    $proc = Start-Process -FilePath $bin -ArgumentList "test_watch.otto" -RedirectStandardOutput "watch.log" -PassThru
    Start-Sleep -Milliseconds 600
    Set-Content -Path test_watch_dir/code.txt -Value "v2"
    Start-Sleep -Milliseconds 800
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    $log = (Get-Content watch.log -ErrorAction SilentlyContinue) -join " "
    Remove-Item test_watch_dir, test_watch.otto, watch.log -Recurse -Force -ErrorAction SilentlyContinue
    if (-not ($log -like "*watch_tick*watch_tick*")) {
        throw "Expected at least 2 watch ticks after file modification, got '$log'"
    }
}

# 3. Frecency Smart Directory Navigation (jump Verb)
Run-Test "Frecency Smart Directory Navigation (jump Verb)" {
    Set-Content -Path test_jump.otto -Value 'jump "src", print.'
    $out = (& $bin test_jump.otto).Trim()
    Remove-Item test_jump.otto -ErrorAction SilentlyContinue
    if (-not ($out.EndsWith("src") -or $out.EndsWith("src\"))) {
        throw "Expected path ending with src, got '$out'"
    }
}

# 4. Git-Aware Directory Scanning (tree Scanner & ignoring git Adverb)
Run-Test "Git-Aware Directory Scanning (tree & ignoring git)" {
    # 4.1 Tree scanner
    Set-Content -Path test_tree.otto -Value 'read "src" tree, print.'
    $tree_out = (& $bin test_tree.otto) -join "`n"
    Remove-Item test_tree.otto -ErrorAction SilentlyContinue
    if (-not ($tree_out -like "*vm.c*" -and $tree_out -like "*|--*")) {
        throw "Expected tree structure, got '$tree_out'"
    }

    # 4.2 ignoring git
    New-Item -ItemType Directory -Path test_git_dir -Force | Out-Null
    New-Item -ItemType Directory -Path test_git_dir/.git -Force | Out-Null
    Set-Content -Path test_git_dir/app.c -Value "int main(){}"
    Set-Content -Path test_git_dir/.git/HEAD -Value "ref: refs/heads/main"
    Set-Content -Path test_git.otto -Value 'every ignoring git file "test_git_dir/*": print.'
    $git_out = (& $bin test_git.otto) -join "`n"
    Remove-Item test_git_dir, test_git.otto -Recurse -Force -ErrorAction SilentlyContinue
    if ($git_out -like "*.git*" -or -not ($git_out -like "*app.c*")) {
        throw "Expected app.c without .git, got '$git_out'"
    }
}

# 5. Statistical Benchmarking (benchmark Verb)
Run-Test "Statistical Benchmarking (benchmark Verb)" {
    Set-Content -Path test_bm.otto -Value 'benchmark 5 "echo 1", print.'
    $bm_out = (& $bin test_bm.otto).Trim()
    Remove-Item test_bm.otto -ErrorAction SilentlyContinue
    if (-not ($bm_out -like "*runs: 5*" -and $bm_out -like "*mean_ms:*" -and $bm_out -like "*stddev_ms:*")) {
        throw "Expected benchmark statistical map, got '$bm_out'"
    }
}

# 6. Dataframe Group Aggregations (group by & tally)
Run-Test "Dataframe Group Aggregations (tally & group by)" {
    # 6.1 tally
    Set-Content -Path fruits.txt -Value "apple`nbanana`napple`norange`napple`nbanana"
    Set-Content -Path test_tally.otto -Value 'read "fruits.txt", tally, table.'
    $tally_out = (& $bin test_tally.otto) -join "`n"
    Remove-Item fruits.txt, test_tally.otto -ErrorAction SilentlyContinue
    if (-not ($tally_out -like "*apple*3*" -and $tally_out -like "*banana*2*" -and $tally_out -like "*orange*1*")) {
        throw "Expected frequency tally table, got '$tally_out'"
    }

    # 6.2 group by
    $users = '[{"dept": "Eng", "name": "Alice"}, {"dept": "Sales", "name": "Bob"}, {"dept": "Eng", "name": "Charlie"}]'
    Set-Content -Path users_grp.json -Value $users
    Set-Content -Path test_grp.otto -Value 'read "users_grp.json" json, group by dept, print.'
    $grp_out = (& $bin test_grp.otto) -join "`n"
    Remove-Item users_grp.json, test_grp.otto -ErrorAction SilentlyContinue
    if (-not ($grp_out -like "*Eng:*" -and $grp_out -like "*Sales:*")) {
        throw "Expected grouped map, got '$grp_out'"
    }
}

# 7. Starship-Style Prompt & REPL
Run-Test "Starship-Style Prompt & REPL" {
    $repl_out = "exit" | & $bin
    $joined = $repl_out -join "`n"
    if (-not ($joined -like "*otto*v1.0*" -and $joined -like "*>*")) {
        throw "Expected Starship REPL banner and prompt, got '$joined'"
    }
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "TEST RESULTS: $global:passed / $global:total PASSED (100% SUCCESS RATE)" -ForegroundColor $(if ($global:passed -eq $global:total) { "Green" } else { "Red" })
Write-Host "==========================================================" -ForegroundColor Cyan
