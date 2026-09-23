param([string]$InputFile, [string]$OutputFile)
$ErrorActionPreference = 'Stop'
# 実行時の CreateDecompressor(COMPRESS_ALGORITHM_MSZIP) と対になるバッファモードで圧縮する
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class MszipCompressor {
    const uint COMPRESS_ALGORITHM_MSZIP = 2;
    [DllImport("cabinet.dll", SetLastError = true)]
    static extern bool CreateCompressor(uint algorithm, IntPtr routines, out IntPtr handle);
    [DllImport("cabinet.dll", SetLastError = true)]
    static extern bool Compress(IntPtr handle, byte[] src, UIntPtr srcSize, byte[] dst, UIntPtr dstSize, out UIntPtr outSize);
    [DllImport("cabinet.dll")]
    static extern bool CloseCompressor(IntPtr handle);

    public static byte[] Run(byte[] src) {
        IntPtr handle;
        if (!CreateCompressor(COMPRESS_ALGORITHM_MSZIP, IntPtr.Zero, out handle)) {
            throw new Win32Exception();
        }
        try {
            UIntPtr size;
            // 出力バッファ無しで呼ぶと必要サイズが返る
            Compress(handle, src, (UIntPtr)(ulong)src.Length, null, UIntPtr.Zero, out size);
            byte[] dst = new byte[(ulong)size];
            if (!Compress(handle, src, (UIntPtr)(ulong)src.Length, dst, (UIntPtr)(ulong)dst.Length, out size)) {
                throw new Win32Exception();
            }
            Array.Resize(ref dst, (int)(ulong)size);
            return dst;
        }
        finally {
            CloseCompressor(handle);
        }
    }
}
'@
[IO.File]::WriteAllBytes($OutputFile, [MszipCompressor]::Run([IO.File]::ReadAllBytes($InputFile)))
