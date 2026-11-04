// pele と KaTeX をビルドの時に QuickJS のバイトコードにする。実行時のコンパイル (約 60ms と約 20ms) を最初の図と数式から省き、
// バイトコードはヒープに写さず exe のリソースから読む。
// js_compile <入力> <出力> module|global
#include <quickjs.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) {
        std::fputs("usage: js_compile <input> <output> module|global\n", stderr);
        return 2;
    }
    const std::filesystem::path input = argv[1];
    // モジュールの名前はバイトコードに残るので、ビルドの場所に依らないファイル名にする。
    const std::string name = input.filename().string();
    std::ifstream in(input, std::ios::binary);
    // JS_Eval は末尾の NUL を要る (std::string が持つ)。
    const std::string source{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    if (!in.is_open() || source.empty()) {
        std::fprintf(stderr, "js_compile: cannot read %s\n", name.c_str());
        return 1;
    }
    const int eval_type = std::wstring_view{ argv[3] } == L"module" ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = rt ? JS_NewContext(rt) : nullptr;
    if (!ctx) {
        return 1;
    }
    JSValue compiled = JS_Eval(ctx, source.c_str(), source.size(), name.c_str(), eval_type | JS_EVAL_FLAG_COMPILE_ONLY);
    int status = 1;
    if (JS_IsException(compiled)) {
        JSValue error = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, error);
        std::fprintf(stderr, "js_compile: %s: %s\n", name.c_str(), message ? message : "error");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    else {
        // 位置の情報を除く (pele で 2.0MB → 0.95MB)。pele も KaTeX も文言に位置を使わない。
        size_t size = 0;
        uint8_t* bytes = JS_WriteObject(ctx, &size, compiled, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG);
        if (bytes) {
            std::ofstream out(std::filesystem::path{ argv[2] }, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
            status = out ? 0 : 1;
            js_free(ctx, bytes);
        }
    }
    JS_FreeValue(ctx, compiled);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return status;
}
