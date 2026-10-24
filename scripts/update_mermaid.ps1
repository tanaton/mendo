<#
.SYNOPSIS
    third_party/mermaid を npm の公式パッケージの ESM 版に入れ替える。

.DESCRIPTION
    npm レジストリから tarball を取り、レジストリが公開する integrity (SHA-512) で検証してから、
    dist/mermaid.esm.min.mjs と dist/chunks/mermaid.esm.min/*.mjs だけを取り出して置き換える。
    チャンクのファイル名は版ごとに変わるので、既存の .mjs はすべて消してから入れ直す。

.EXAMPLE
    pwsh scripts/update_mermaid.ps1            # 最新版
    pwsh scripts/update_mermaid.ps1 12.2.0     # 版を指定
#>
param(
    [string]$Version = 'latest'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$dest = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party/mermaid'
$entryName = 'mermaid.esm.min.mjs'
$chunkDir = 'chunks/mermaid.esm.min'

$meta = Invoke-RestMethod "https://registry.npmjs.org/mermaid/$Version"
$resolved = $meta.version
Write-Host "mermaid $resolved"

$work = Join-Path ([IO.Path]::GetTempPath()) "mendo-mermaid-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory $work | Out-Null
try {
    $tgz = Join-Path $work 'mermaid.tgz'
    Invoke-WebRequest $meta.dist.tarball -OutFile $tgz

    if ($meta.dist.integrity -notmatch '^sha512-(.+)$') {
        throw "想定外の integrity 形式: $($meta.dist.integrity)"
    }
    $expected = [BitConverter]::ToString([Convert]::FromBase64String($Matches[1])).Replace('-', '')
    if ((Get-FileHash $tgz -Algorithm SHA512).Hash -ne $expected) {
        throw "tarball の SHA-512 がレジストリの integrity と一致しない"
    }

    $out = Join-Path $work 'out'
    New-Item -ItemType Directory $out | Out-Null
    # npm の tarball は全ファイルの時刻が 1985 年に固定されていて、そのままだとビルドが埋め込みを作り直さない (-m)
    & "$env:SystemRoot\System32\tar.exe" -xmzf $tgz -C $out "package/dist/$entryName" "package/dist/$chunkDir"
    if ($LASTEXITCODE -ne 0) {
        throw "tar.exe が失敗した (exit $LASTEXITCODE)。この版は ESM 版の配置が違うかもしれない"
    }

    $dist = Join-Path $out 'package/dist'
    $relPaths = @{}
    Get-ChildItem $dist -Recurse -File | Where-Object Extension -eq '.mjs' | ForEach-Object {
        $relPaths[$_.FullName.Substring($dist.Length + 1).Replace('\', '/')] = $_
    }
    if (-not $relPaths.ContainsKey($entryName)) {
        throw "$entryName が見つからない"
    }

    # 埋め込んだファイルだけで import がすべて解決できるか確かめる
    foreach ($rel in $relPaths.Keys) {
        $baseDir = [IO.Path]::GetDirectoryName($rel).Replace('\', '/')
        $text = [IO.File]::ReadAllText($relPaths[$rel].FullName)
        foreach ($m in [regex]::Matches($text, '(?:from|import)\s*\(?\s*"\./([^"]+\.mjs)"')) {
            $target = if ($baseDir) { "$baseDir/$($m.Groups[1].Value)" } else { $m.Groups[1].Value }
            if (-not $relPaths.ContainsKey($target)) {
                throw "$rel が読み込む $target が含まれていない"
            }
        }
    }

    if (Test-Path (Join-Path $dest 'chunks')) {
        Remove-Item (Join-Path $dest 'chunks') -Recurse -Force
    }
    Get-ChildItem $dest -File -Filter '*.mjs' | Remove-Item -Force
    foreach ($rel in $relPaths.Keys) {
        $to = Join-Path $dest $rel
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($to)) | Out-Null
        Copy-Item $relPaths[$rel].FullName $to
    }
    Write-Host "$($relPaths.Count) 個のファイルを $dest に置いた"
}
finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host @"

次の手順:
  1. cmake --build build --config Release
  2. example/test.md のライブラリ一覧にある mermaid.js の版を $resolved にする
"@
