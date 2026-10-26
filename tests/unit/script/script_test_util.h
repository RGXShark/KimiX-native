// Shared helpers for the kimix-script test suites (unit/script/*).
// This header is NOT a registered test target; every helper is inline so the
// three suites can include it without colliding.
//
// What it provides:
// - has_long_hex(): the pointer-hygiene audit ("no run of 8+ hex digits").
// - stderr_capture: redirect the CRT's stderr into a temp file for the duration
//   of a scope, then read it back, so "the sandbox wrote nothing to stderr" is an
//   assertion instead of a claim.
// - kApiScripts: the 17 forbidden APIs of the hostile matrix, each with the Lua
//   expression that reaches it.
#pragma once
#ifndef KIMIX_SCRIPT_TEST_UTIL_H
#define KIMIX_SCRIPT_TEST_UTIL_H

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#define KIMIX_SCRIPT_DUP _dup
#define KIMIX_SCRIPT_DUP2 _dup2
#define KIMIX_SCRIPT_CLOSE _close
#define KIMIX_SCRIPT_FILENO _fileno
#else
#include <unistd.h>
#define KIMIX_SCRIPT_DUP dup
#define KIMIX_SCRIPT_DUP2 dup2
#define KIMIX_SCRIPT_CLOSE close
#define KIMIX_SCRIPT_FILENO fileno
#endif

#include <core/stl/string.h>

namespace script_test {

/* true when `text` contains a run of at least `min_run` hexadecimal digits --
 * i.e. something that looks like an address (or a dumped pointer). */
inline bool has_long_hex(kimix::string_view text, size_t min_run = 8) {
    size_t run = 0;
    for (char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (hex) {
            if (++run >= min_run) {
                return true;
            }
        } else {
            run = 0;
        }
    }
    return false;
}

/* Redirect stderr into a temp file for a scope, then read it back.  Used by the
 * suites that must PROVE the sandbox produced nothing on the process streams
 * (a Lua warning, a panic, a leak report would all show up here). */
class stderr_capture {
public:
    stderr_capture() = default;
    stderr_capture(const stderr_capture &) = delete;
    stderr_capture &operator=(const stderr_capture &) = delete;

    ~stderr_capture() { (void)finish(); }

    bool begin() {
        if (_active) {
            return true;
        }
        _tmp = std::tmpfile();
        if (_tmp == nullptr) {
            return false;
        }
        std::fflush(stderr);
        _saved = KIMIX_SCRIPT_DUP(2);
        if (_saved < 0) {
            std::fclose(_tmp);
            _tmp = nullptr;
            return false;
        }
        KIMIX_SCRIPT_DUP2(KIMIX_SCRIPT_FILENO(_tmp), 2);
        _active = true;
        return true;
    }

    kimix::string finish() {
        kimix::string captured;
        if (!_active || _tmp == nullptr) {
            return captured;
        }
        std::fflush(stderr);
        KIMIX_SCRIPT_DUP2(_saved, 2);
        KIMIX_SCRIPT_CLOSE(_saved);
        _saved = -1;
        _active = false;
        if (std::fseek(_tmp, 0, SEEK_END) == 0) {
            const long size = std::ftell(_tmp);
            if (size > 0) {
                captured.resize(static_cast<size_t>(size));
                std::fseek(_tmp, 0, SEEK_SET);
                const size_t read = std::fread(&captured[0], 1, static_cast<size_t>(size), _tmp);
                captured.resize(read);
            }
        }
        std::fclose(_tmp);
        _tmp = nullptr;
        return captured;
    }

private:
    FILE *_tmp = nullptr;
    int _saved = -1;
    bool _active = false;
};

/* The forbidden capability corpus of the plan (§9 S-V3): 16 named APIs plus the
 * bytecode loader, each with the Lua expression that reaches it.  `api` is the
 * exact string the violation record and the DENIED message must carry. */
struct forbidden_api {
    const char *api;
    const char *category;
    const char *expr; /* the Lua expression that triggers the denial */
};

inline const forbidden_api *forbidden_apis(size_t *count) {
    static const forbidden_api kApis[] = {
        {"io.open", "file-io", "io.open('x.txt', 'r')"},
        {"io.popen", "file-io", "io.popen('echo hi')"},
        {"os.execute", "process", "os.execute('echo hi')"},
        {"os.exit", "process", "os.exit(0)"},
        {"os.remove", "process", "os.remove('x.txt')"},
        {"os.rename", "process", "os.rename('a.txt', 'b.txt')"},
        {"os.tmpname", "process", "os.tmpname()"},
        {"os.getenv", "env", "os.getenv('PATH')"},
        {"os.setlocale", "env", "os.setlocale('C')"},
        {"package.loadlib", "module", "package.loadlib('a.dll', 'b')"},
        {"require", "module", "require('nope')"},
        {"debug.getregistry", "introspection", "debug.getregistry()"},
        {"debug.upvalueid", "introspection", "debug.upvalueid(print, 1)"},
        {"dofile", "file-io", "dofile('x.lua')"},
        {"loadfile", "file-io", "loadfile('x.lua')"},
        {"string.dump", "bytecode", "string.dump(function() end)"},
        {"load", "bytecode", "load('\\27Lua' .. string.rep('\\0', 8))"},
    };
    *count = sizeof(kApis) / sizeof(kApis[0]);
    return kApis;
}

} // namespace script_test

#endif /* KIMIX_SCRIPT_TEST_UTIL_H */
