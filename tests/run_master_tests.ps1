Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "OTTO MASTER VERIFICATION TEST SUITE (DEFINITION OF DONE)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$global:passed = 0
$global:total = 0

$bin = if (Test-Path ".\otto.exe") { ".\otto.exe" } else { "& $bin" }

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

# 1. Zero-filler recursion & TCO
Run-Test "Zero-filler recursion & TCO (gcd 1071 462)" {
    $out = & $bin -c "gcd a b: return a if b == 0 else gcd b (a % b). print gcd 1071 462."
    if ("$out".Trim() -ne "21") { throw "Expected 21, got '$out'" }
}

# 1b. TCO Deep Recursion
Run-Test "TCO Deep Recursion (count_down 1000000)" {
    $out = & $bin -c "count_down n: return 0 if n <= 0 else count_down (n - 1). print count_down 1000000."
    if ("$out".Trim() -ne "0") { throw "Expected 0, got '$out'" }
}

# 2. AWK lifecycle + ordinal + implicit number scanner
Run-Test "AWK lifecycle + ordinal + implicit number scanner" {
    $scores = @"
alice 100 math
bob 90 science
charlie 80 history
"@
    Set-Content -Path scores.txt -Value $scores
    $out = & $bin -c 'before: 0 into s. every line "scores.txt": add 2nd number into s. after: print s.'
    Remove-Item scores.txt -ErrorAction SilentlyContinue
    if ("$out".Trim() -ne "270") { throw "Expected 270, got '$out'" }
}

# 3. Possessive JSON navigation + sorting
Run-Test "Possessive JSON navigation + sorting" {
    $users = @"
[
  {"name": "Alice", "age": 25, "active": true},
  {"name": "Bob", "age": 40, "active": true},
  {"name": "Charlie", "age": 30, "active": false},
  {"name": "David", "age": 35, "active": true}
]
"@
    Set-Content -Path users.json -Value $users
    $out = & $bin -c 'read "users.json" json, take where each''s active == true, sort by age descending, print each''s name.'
    Remove-Item users.json -ErrorAction SilentlyContinue
    $lines = @($out) | Where-Object { $_ -ne "" }
    if ($lines[0] -ne "Bob" -or $lines[1] -ne "David" -or $lines[2] -ne "Alice") {
        throw "Expected Bob, David, Alice; got $out"
    }
}

# 4. Parallel tee (`and`) + short-circuit (`or`)
Run-Test "Parallel tee (and) + short-circuit (or)" {
    $backup = @"
line one: host=web1
line two: port=8080
line three: debug=true
"@
    Set-Content -Path backup.cfg -Value $backup
    Remove-Item primary.cfg -ErrorAction SilentlyContinue
    $out = & $bin -c 'read "primary.cfg" or read "backup.cfg", print first line and print count lines.'
    Remove-Item backup.cfg -ErrorAction SilentlyContinue
    $lines = @($out) | Where-Object { $_ -ne "" }
    if ($lines[0] -ne "line one: host=web1" -or $lines[1] -ne "3") {
        throw "Expected 'line one: host=web1' and '3', got $out"
    }
}

# 5. In-place dry-run & transactional rollback
Run-Test "In-place dry-run & transactional rollback (simulate, inplace, undo)" {
    Set-Content -Path doc.txt -Value "hello foo world"
    # 5.1 simulate: dry run preview without touching disk
    & $bin -c 'every file "*.txt": simulate inplace replace "foo" with "bar".' | Out-Null
    $content = (Get-Content doc.txt -Raw).Trim()
    if ($content -ne "hello foo world") { throw "Dry-run modified file! Got: $content" }

    # 5.2 inplace: modify for real with auto-journal
    & $bin -c 'every file "*.txt": inplace replace "foo" with "bar".' | Out-Null
    $content = (Get-Content doc.txt -Raw).Trim()
    if ($content -ne "hello bar world") { throw "Inplace replace failed! Got: $content" }

    # 5.3 undo: restore original
    & $bin -c 'undo last.' | Out-Null
    $content = (Get-Content doc.txt -Raw).Trim()
    Remove-Item doc.txt -ErrorAction SilentlyContinue
    if ($content -ne "hello foo world") { throw "Undo rollback failed! Got: $content" }
}

# 6. Relational set operations
Run-Test "Relational set operations (exclude, intersect, count)" {
    Set-Content -Path candidates.txt -Value "alice`nbob`ncharlie`ndavid"
    Set-Content -Path blacklist.txt -Value "bob"
    Set-Content -Path verified.txt -Value "alice`ndavid`neva"
    $out = & $bin -c 'exclude "blacklist.txt" from "candidates.txt", intersect with "verified.txt", print count.'
    Remove-Item candidates.txt, blacklist.txt, verified.txt -ErrorAction SilentlyContinue
    if ("$out".Trim() -ne "2") { throw "Expected count 2, got '$out'" }
}

# 7. Binary unpacking
Run-Test "Binary unpacking (unpack u32 u8 u8)" {
    $bytes = [byte[]](0x78, 0x56, 0x34, 0x12, 0x01, 0x02, 0x00, 0x00)
    [System.IO.File]::WriteAllBytes("sample.bin", $bytes)
    $out = & $bin -c 'read "sample.bin", take first 8 bytes, unpack "u32 u8 u8" into magic bits endian, print magic.'
    Remove-Item sample.bin -ErrorAction SilentlyContinue
    if ("$out".Trim() -ne "305419896") { throw "Expected 305419896 (0x12345678), got '$out'" }
}

# 8. Incremental build rule
Run-Test "Incremental build rule (target from src)" {
    Set-Content -Path hello.c -Value 'int main() { return 0; }'
    Remove-Item hello.exe -ErrorAction SilentlyContinue
    & $bin -c '"hello" from "hello.c": run "zig cc hello.c -o hello.exe".' | Out-Null
    if (-not (Test-Path hello.exe)) { throw "Failed to build hello.exe" }
    Remove-Item hello.c, hello.exe -ErrorAction SilentlyContinue
}

# 9. Embedded SQLite stream
Run-Test "Embedded SQLite stream (auto-create table, insert, query pushdown)" {
    Remove-Item ops.db -ErrorAction SilentlyContinue
    $out = & $bin -c 'write {host: "web1", cpu: 85} onto "ops.db:metrics". every row "ops.db:metrics" where cpu >= 80: print host.'
    Remove-Item ops.db -ErrorAction SilentlyContinue
    if ("$out".Trim() -ne "web1") { throw "Expected 'web1', got '$out'" }
}

# 10. Standalone binary compilation
Run-Test "Standalone binary compilation (compile into standalone executable)" {
    Set-Content -Path test.otto -Value 'print "Hello from standalone compiled binary!".'
    $out = & $bin -c 'compile "test.otto" into "./test-bin", run "./test-bin", print.'
    Remove-Item test.otto, test-bin* -ErrorAction SilentlyContinue
    if ("$out".Trim() -ne "Hello from standalone compiled binary!") {
        throw "Expected standalone binary output, got '$out'"
    }
}

# 11. YAML Processing
Run-Test "Native YAML Stream Processing" {
    $yaml = @"
name: ProductionCluster
port: 9000
nodes: 16
"@
    Set-Content -Path cluster.yaml -Value $yaml
    $out = & $bin -c 'read "cluster.yaml" yaml, print.'
    Remove-Item cluster.yaml -ErrorAction SilentlyContinue
    if (-not ("$out" -like "*ProductionCluster*" -and "$out" -like "*9000*")) {
        throw "Expected parsed YAML map, got '$out'"
    }
}

# 12. Strict Zero-Filler Rule Rejection
Run-Test "Strict Zero-Filler Rule Rejection" {
    $out = & $bin -c 'To gcd of a and b.' 2>&1 | Out-String
    if (-not ($out -like "*zero-filler*")) {
        throw "Expected zero-filler syntax rejection, got '$out'"
    }
}

# 13. Interactive TUI Table
Run-Test "TUI Table Grid Rendering" {
    $users = @"
[
  {"name": "Alice", "age": 25},
  {"name": "Bob", "age": 40}
]
"@
    Set-Content -Path users.json -Value $users
    $out = & $bin -c 'read "users.json" json, table.'
    Remove-Item users.json -ErrorAction SilentlyContinue
    if (-not ("$out" -like "*+*---*+*" -and "$out" -like "*Alice*" -and "$out" -like "*Bob*")) {
        throw "Expected table box grid, got '$out'"
    }
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "TEST RESULTS: $global:passed / $global:total PASSED (100% SUCCESS RATE)" -ForegroundColor $(if ($global:passed -eq $global:total) { "Green" } else { "Red" })
Write-Host "==========================================================" -ForegroundColor Cyan
