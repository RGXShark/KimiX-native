/*
 * script_hook.cpp -- the resource watchdog (L4): deadline, instruction budget,
 * and host cancellation.
 *
 * One LUA_MASKCOUNT hook is installed on the main thread BEFORE any script code
 * runs.  lua_newthread copies hookmask/hook/basehookcount, so every coroutine a
 * script creates inherits it -- that is what makes `while true do ... end` inside
 * a coroutine as interruptible as a top-level loop.  A script cannot remove it
 * either: lua_sethook is C-only, and `debug` is a tripwire.
 *
 * The hook body is short and allocation-free apart from the error message it
 * composes when it fires (which goes through the state's fixed body buffer).
 * It cannot stop a script that catches the error -- that is the re-raising
 * wrappers' job (script_env.cpp); this file only has to raise.
 */
#include <script/script_lua.h>
#include <script/script_error.h>
#include <script/script_state.h>

namespace kimix::script {

void sb_hook(lua_State *L, lua_Debug *ar) noexcept {
    (void)ar;
    SandboxState *s = sb_from(L);
    s->instructions += (s->hook_granularity > 0) ? s->hook_granularity : 1;

    if (s->cancel.load(std::memory_order_relaxed)) {
        sb_raisef(L, Status::cancelled, "cancel", "the host cancelled this run (%llu instructions executed)",
                  static_cast<unsigned long long>(s->instructions));
        return;
    }
    if (s->deadline_s > 0.0 && s->clock.toc_seconds() > s->deadline_s) {
        sb_raisef(L, Status::timeout, "time", "script exceeded its %.3f s deadline (%llu instructions)",
                  s->deadline_s, static_cast<unsigned long long>(s->instructions));
        return;
    }
    if (s->max_instructions != 0 && s->instructions > s->max_instructions) {
        sb_raisef(L, Status::limit, "instruction", "script exceeded its %llu-instruction budget",
                  static_cast<unsigned long long>(s->max_instructions));
    }
}

} // namespace kimix::script
