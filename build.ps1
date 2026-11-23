<#
  build.ps1 -- 编译 + 运行，一条命令搞定
  ============================================================

  为什么有这个脚本？
    原来每次要手敲一长串：
      g++ -std=c++17 -Wall -Wextra -Iinclude src/demo_wal.cpp -o build_win/demo_wal.exe; if ($?) { .\build_win\demo_wal.exe }
    敲五次会有一次拼错，所以固定成脚本。

  用法（在 D:\code\lsm-kv 下）：
    .\build.ps1 list                看看有哪些 demo
    .\build.ps1 all                 全部重新编译（改了 .h 之后必须用这个）
    .\build.ps1 demo_wal            编译并运行 demo_wal
    .\build.ps1 demo_crash dirty    编译 demo_crash，带参数 dirty 运行
    .\build.ps1 demo_crash "bulk 1000 347"
    .\build.ps1 demo_skip -dbg     带调试信息编译，并在 gdb 里跑（崩溃时直接给出出错行）
    .\build.ps1 tskip               跑 tests/test_skip.cpp（你自己写的单测）
    .\build.ps1 mutants             拿你的单测去跑 6 个故意写错的实现，看能抓几个
#>

param(
    [string]$target = "list",
    [string]$runArgs = "",
    [switch]$dbg
)

# ── 控制台切到 UTF-8 ────────────────────────────────────────
#
# 为什么需要这一句：
#   源码是 UTF-8，g++ 默认把字符串字面量按 UTF-8 编进 exe，
#   于是程序 printf 出来的是 UTF-8 字节。
#   但中文 Windows 的控制台默认代码页是 936（GBK），
#   拿 GBK 去解 UTF-8 字节 → 「验收」显示成「楠屾敹」。
#
# chcp 65001 = 让这个终端按 UTF-8 解码。
#
# ★ 只影响当前这个终端会话。新开一个终端又会回到 936，
#   除非在 VS Code 的 terminal.integrated.profiles 里也加上。
& chcp.com 65001 > $null 2>&1

$demoNames = @(
    "demo_record",
    "demo_crc32",
    "demo_file",
    "demo_load",
    "demo_wal",
    "demo_log",
    "demo_corrupt",
    "demo_crash",
    "demo_skip",
    "demo_memtable",
    "demo_memset",
    "demo_ownership",
    "demo_slice",
    "demo_lifetime",
    "demo_flush_cost",
    "demo_sstable",
    "demo_restart",
    "demo_wal_db"
)

# 编译参数。要改标准或加优化，改这一行。
$flags = @("-std=c++17", "-Wall", "-Wextra", "-Iinclude")

# 有些 demo 不止一个 .cpp（比如 demo_restart 要连 db_sst.cpp 一起编）。
# 没写在这里的，默认就是 src/<name>.cpp 单文件。
$extraSources = @{
    "demo_restart" = @("src/db_sst.cpp")
    "demo_wal_db"  = @("src/db_sst.cpp")
}

function SrcsOf([string]$name) {
    $s = @("src/$name.cpp")
    if ($extraSources.ContainsKey($name)) { $s += $extraSources[$name] }
    return , $s
}

# 编译一个。返回 $true/$false。
# 用 Out-Host 让 g++ 的警告直接显示在终端，而不是被当成函数返回值吞掉。
function Build-One([string]$name, [string]$suffix = "", [string[]]$extra = @(), [string[]]$sources = @()) {
    if ($sources.Count -eq 0) { $sources = @("src/$name.cpp") }
    & g++ @flags @extra @sources -o "build_win/$name$suffix.exe" | Out-Host
    return ($LASTEXITCODE -eq 0)
}

if ($target -eq "list") {
    Write-Host "available demos:"
    foreach ($n in $demoNames) {
        Write-Host "  $n"
    }
    Write-Host ""
    Write-Host "usage: .\build.ps1 <name> [args]"
    exit 0
}

if ($target -eq "all") {
    $bad = 0
    foreach ($n in $demoNames) {
        Write-Host ("building {0} ... " -f $n) -NoNewline
        if (Build-One $n "" @() (SrcsOf $n)) {
            Write-Host "ok"
        } else {
            Write-Host "FAILED" -ForegroundColor Red
            $bad++
        }
    }
    if ($bad -gt 0) {
        Write-Host ("{0} demo(s) failed" -f $bad) -ForegroundColor Red
        exit 1
    }
    Write-Host ("all {0} demos built" -f $demoNames.Count)
    exit 0
}

# 编译一个 tests/ 下的文件。
# $mutantDir 非空时，把那个目录放到 -Iinclude 之前 —— 它里面的 lsmkv/skip_list.h
# 会顶掉正式版本，这就是变异体生效的原理（g++ 取第一个匹配到的头文件）。
function Build-Test([string]$src, [string]$out, [string]$mutantDir = "") {
    $a = @("-std=c++17", "-Wall", "-Wextra")
    if ($mutantDir -ne "") { $a += ("-I" + $mutantDir) }
    $a += @("-Iinclude")
    & g++ @a $src -o $out 2>&1 | Out-Host
    return ($LASTEXITCODE -eq 0)
}

# ── 单测：你自己写的 tests/test_skip.cpp ───────────────────
if ($target -eq "tskip") {
    if (-not (Build-Test "tests/test_skip.cpp" "build_win/test_skip.exe")) { exit 1 }
    & ".\build_win\test_skip.exe"
    exit $LASTEXITCODE
}

# ── 变异体检测：拿你的测试去跑 6 个错误实现 ────────────────
if ($target -eq "mutants") {
    Write-Host "real implementation:" -NoNewline
    if (-not (Build-Test "tests/test_skip.cpp" "build_win/tskip_real.exe")) {
        Write-Host " compile error" -ForegroundColor Red
        exit 1
    }
    & ".\build_win\tskip_real.exe" | Out-Host
    $realOk = ($LASTEXITCODE -eq 0)
    if ($realOk) {
        Write-Host "  PASS   your tests are green on correct code" -ForegroundColor Green
    } else {
        Write-Host "  FAIL   your tests are RED on CORRECT code -- fix your tests first" -ForegroundColor Red
    }

    Write-Host ""
    Write-Host "broken implementations:" -NoNewline
    Write-Host ""
    $caught = 0
    foreach ($m in @("m1", "m2", "m3", "m4", "m5", "m6")) {
        $dir = "tests/mutants/$m"
        $exe = "build_win/tskip_$m.exe"
        if (-not (Build-Test "tests/test_skip.cpp" $exe $dir)) {
            Write-Host ("  {0}  COMPILE ERROR" -f $m) -ForegroundColor Red
            continue
        }
        & ".\$exe" > $null 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("  {0}  caught" -f $m) -ForegroundColor Green
            $caught++
        } else {
            Write-Host ("  {0}  MISSED   <- a class of bugs walks right through your tests" -f $m) -ForegroundColor Yellow
        }
    }
    Write-Host ""
    Write-Host ("score: {0} / 6" -f $caught)
    if ($caught -eq 6 -and $realOk) {
        Write-Host "done -- this is what a test suite is for." -ForegroundColor Green
        exit 0
    }
    exit 1
}

# ── 迭代器测试：tests/test_iter.cpp ─────────────────────────
# TODO 9（顺序遍历）+ TODO 13（Seek）
if ($target -eq "titer") {
    if (-not (Build-Test "tests/test_iter.cpp" "build_win/test_iter.exe")) { exit 1 }
    & ".\build_win\test_iter.exe"
    exit $LASTEXITCODE
}

# ── 接口测试：tests/test_basic.cpp 跑当前 DB 实现 ───────────
# 这是 Week 4 的验收线：DB 接口不变，实现从 std::map 换成 MemTableSet。
if ($target -eq "tbasic") {
    & g++ @flags "tests/test_basic.cpp" "src/db_mem.cpp" -o "build_win/test_basic.exe" 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & ".\build_win\test_basic.exe"
    exit $LASTEXITCODE
}

if ($demoNames -notcontains $target) {
    Write-Host ("unknown demo: {0}" -f $target) -ForegroundColor Red
    Write-Host "run .\build.ps1 list to see what is available"
    exit 1
}

$suffix = ""
$extra = @()
if ($dbg) {
    $suffix = "_dbg"
    $extra = @("-g", "-O0")
}

if (-not (Build-One $target $suffix $extra (SrcsOf $target))) {
    Write-Host "build failed, not running the old exe" -ForegroundColor Red
    exit 1
}

$exe = ".\build_win\$target$suffix.exe"

if ($dbg) {
    # gdb 的 -batch = 跑完就退出；-ex run 启动程序；-ex bt 崩溃后打印调用栈
    $gdb = "C:\mg\mingw64\bin\gdb.exe"
    if (-not (Test-Path $gdb)) { $gdb = "gdb" }
    Write-Host ""
    Write-Host "running under gdb -- 崩溃后看 bt 里第一条属于你代码的栈帧" -ForegroundColor Yellow
    Write-Host ""
    if ($runArgs -ne "") {
        & $gdb -batch -ex run -ex bt --args $exe $runArgs.Split(" ")
    } else {
        & $gdb -batch -ex run -ex bt --args $exe
    }
    exit 0
}

if ($runArgs -ne "") {
    $parts = $runArgs.Split(" ")
    & $exe $parts
} else {
    & $exe
}

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host ("程序退出码 {0}（非 0）。" -f $LASTEXITCODE) -ForegroundColor Yellow
    Write-Host "  -1073741819 = 0xC0000005 = 访问了非法内存（空指针 / 越界）" -ForegroundColor Yellow
    Write-Host "  想直接看到崩溃的那一行： .\build.ps1 $target -dbg" -ForegroundColor Yellow
}
exit $LASTEXITCODE
