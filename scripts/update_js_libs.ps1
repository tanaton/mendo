<#
.SYNOPSIS
    third_party/pele と third_party/katex を npm の公式パッケージから入れ替える。

.DESCRIPTION
    npm レジストリから tarball を取り、レジストリが公開する integrity (SHA-512) で検証してから、
    埋め込むファイルと LICENSE だけを取り出して置き換える。

.EXAMPLE
    pwsh scripts/update_js_libs.ps1                              # どちらも最新版
    pwsh scripts/update_js_libs.ps1 -Pele 1.13.0 -Katex 0.19.1   # 版を指定
#>
param(
    [string]$Pele = 'latest',
    [string]$Katex = 'latest'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$thirdParty = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party'

# $files は tarball 内のパス (package/ より下) → third_party/<name> の下の置き場所。
function Update-NpmPackage([string]$name, [string]$version, [hashtable]$files) {
    $meta = Invoke-RestMethod "https://registry.npmjs.org/$name/$version"
    $resolved = $meta.version
    Write-Host "$name $resolved"

    $work = Join-Path ([IO.Path]::GetTempPath()) "mendo-$name-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory $work | Out-Null
    try {
        $tgz = Join-Path $work "$name.tgz"
        Invoke-WebRequest $meta.dist.tarball -OutFile $tgz

        if ($meta.dist.integrity -notmatch '^sha512-(.+)$') {
            throw "想定外の integrity 形式: $($meta.dist.integrity)"
        }
        $expected = [BitConverter]::ToString([Convert]::FromBase64String($Matches[1])).Replace('-', '')
        if ((Get-FileHash $tgz -Algorithm SHA512).Hash -ne $expected) {
            throw "$name の tarball の SHA-512 がレジストリの integrity と一致しない"
        }

        $out = Join-Path $work 'out'
        New-Item -ItemType Directory $out | Out-Null
        # npm の tarball は全ファイルの時刻が 1985 年に固定されていて、そのままだとビルドが埋め込みを作り直さない (-m)
        $members = @($files.Keys | ForEach-Object { "package/$_" })
        & "$env:SystemRoot\System32\tar.exe" -xmzf $tgz -C $out @members
        if ($LASTEXITCODE -ne 0) {
            throw "tar.exe が失敗した (exit $LASTEXITCODE)。$name $resolved は配置が違うかもしれない"
        }

        $dest = Join-Path $thirdParty $name
        if (Test-Path $dest) {
            Remove-Item $dest -Recurse -Force
        }
        New-Item -ItemType Directory $dest | Out-Null
        foreach ($src in $files.Keys) {
            $target = Join-Path $dest $files[$src]
            New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
            Copy-Item (Join-Path $out "package/$src") $target
        }
    }
    finally {
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    return $resolved
}

# pele は ESM の 1 ファイル版 (全種類の図を含む)。KaTeX は UMD の縮小版 (QuickJS で globalThis.katex になる) と、
# 描画に使う書体 20 個の woff2 (DirectWrite が展開して読む)。書体の一覧は diagram_raster.h の kKatexFontFiles と同じ。
$katexFonts = @(
    'AMS-Regular', 'Caligraphic-Bold', 'Caligraphic-Regular', 'Fraktur-Bold', 'Fraktur-Regular',
    'Main-Bold', 'Main-BoldItalic', 'Main-Italic', 'Main-Regular', 'Math-BoldItalic',
    'Math-Italic', 'SansSerif-Bold', 'SansSerif-Italic', 'SansSerif-Regular', 'Script-Regular',
    'Size1-Regular', 'Size2-Regular', 'Size3-Regular', 'Size4-Regular', 'Typewriter-Regular'
)
$katexFiles = @{ 'dist/katex.min.js' = 'katex.min.js'; 'LICENSE' = 'LICENSE' }
foreach ($font in $katexFonts) {
    $katexFiles["dist/fonts/KaTeX_$font.woff2"] = "fonts/KaTeX_$font.woff2"
}
$peleVersion = Update-NpmPackage 'pele' $Pele @{ 'dist/pele.js' = 'pele.mjs'; 'LICENSE' = 'LICENSE' }
$katexVersion = Update-NpmPackage 'katex' $Katex $katexFiles

Write-Host @"

次の手順:
  1. cmake --build build --config Release
  2. example/test.md のライブラリ一覧にある pele の版を $peleVersion に、KaTeX の版を $katexVersion にする
"@
