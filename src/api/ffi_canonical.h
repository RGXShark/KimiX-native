/*
 * ffi_canonical.h -- deterministic canonical JSON serialisation, exported to C.
 *
 * WHAT THIS MODULE IS FOR
 * -----------------------
 * A digest, a jsonb dedupe key, or an idempotency fingerprint needs a byte
 * string that is THE SAME for every JSON document that means the same thing --
 * independent of member order, of insignificant whitespace, or of how the
 * author spelled a number.  `kimix_canonical_json()` takes any strictly-valid
 * JSON text and re-emits it in one fixed, canonical spelling:
 *
 *   * object members are SORTED by key, ascending, using ORDINAL BYTE order of
 *     the key's decoded UTF-8 bytes (memcmp over the common prefix; the shorter
 *     key wins when one is a prefix of the other).  Arrays keep their order.
 *   * there is NO insignificant whitespace (minified) and NO UTF-8 BOM.
 *   * numbers are emitted as their ORIGINAL text, verbatim: the parser is run
 *     with the raw-number flag so `1.0`, `1e2`, `-0` and `1` each re-print
 *     exactly what was read.  Two spellings that yyjson would coerce to the
 *     same double are therefore NOT folded together -- canonical identity here
 *     is byte-identity of the source number token, which is what the surrounding
 *     plan's "preserve number precision" rule requires (1.0 != 1).
 *   * strings and object keys are re-escaped with the fixed byte table below.
 *
 * THE RESULT CONVENTION (the one thing to remember)
 * --------------------------------------------------
 * On success `*out` is a freshly constructed `kimix_vec` (api/ffi_vec.h) holding
 * EXACTLY the canonical UTF-8 bytes -- no trailing NUL, no BOM.  Unlike
 * kimix_repair() the vector length equals the text length byte-for-byte, so
 * kimix_vec_size() is the canonical length and kimix_vec_data() the first byte.
 * The call never emits "an empty result for a valid document": a valid input
 * always produces at least the bytes of its canonical form (e.g. `{}` or `[]`).
 * Every failure leaves `*out` untouched (never constructed) and returns a
 * non-zero `kimix_status`.
 *
 * OWNERSHIP AND COST
 * ------------------
 * * `out` is a caller-owned RAW placeholder (not yet initialised, or already
 *   destroyed): the call CONSTRUCTS the result vector inside it, exactly like
 *   kimix_repair().  Release it with kimix_vec_destroy() (inline storage) -- the
 *   buffer is one mimalloc allocation made inside this library (see ffi_vec.h).
 * * The input is length-based (`utf8` + `len`), so it need NOT be
 *   NUL-terminated and may contain embedded NUL bytes -- those are simply data
 *   that the strict parser rejects (see the error table).  Nothing on the happy
 *   path depends on a terminator.
 * * Nothing here throws: the project is built without C++ exceptions, and every
 *   failure is a `kimix_status`.
 *
 * THE CANONICAL BYTE GRAMMAR (the whole point of the call -- byte-identical to
 * the managed Semevia.Core fallback, so the two spellings can be cross-checked)
 * ------------------------------------------------------------------------------
 * Values, minified:
 *   object  ->  '{' [ "<key>" ':' <value> [ ',' ... ] ] '}'   members key-sorted
 *   array   ->  '[' <value> [ ',' <value> ... ] ']'            order preserved
 *   string  ->  '"' <escaped-body> '"'                          (keys and values)
 *   number  ->  the original source token, copied verbatim
 *   literals->  'null' / 'true' / 'false'
 * An object with two keys whose decoded UTF-8 bytes are equal is a duplicate key
 * and is REJECTED (see the error table): canonical form is only defined for a
 * function from key to value, so a repeated key is an input error, not something
 * to last-write-wins over.
 *
 * Escape table (applied per byte of the DECODED UTF-8 of a string / key; all
 * other bytes -- and every byte of a valid multi-byte UTF-8 sequence -- are
 * copied through unchanged, so non-ASCII text is never \u-escaped):
 *   '"'   0x22 -> \"        '\'   0x5C -> \\
 *   0x08  -> \b            0x09  -> \t        0x0A -> \n
 *   0x0C  -> \f            0x0D  -> \r
 *   any other 0x00..0x1F   -> \u00xx   (lowercase hex, e.g. U+0000 -> \u0000)
 * Because the strict parser refuses raw control bytes and ill-formed UTF-8, any
 * byte below 0x20 that survives into the decoded string arrived through a
 * `\uXXXX` / `\t` / `\n` escape and is re-escaped back with the table above.
 *
 * INPUT ACCEPTANCE AND ITS ERROR CODES
 * -------------------------------------
 * The input is first parsed with yyjson (the library's mimalloc allocator, see
 * ffi_yyjson.h for why the allocator is hidden) in STRICT RFC 8259 mode plus the
 * raw-number flag -- no comments, no trailing commas, no JSON5 forms, no
 * single-quoted strings.  A document the strict parser refuses is reported as
 * KIMIX_ERR_INVALID_INPUT (never undefined behaviour), which covers:
 *   * malformed JSON / wrong structure / an empty or whitespace-only document
 *     (yyjson refuses a 0-length buffer and content that is not one value),
 *   * a raw NUL byte anywhere (0x00 is a disallowed control character, not a
 *     terminator, because the API is length-based -- this is the binary-safety
 *     guarantee: an embedded NUL is REJECTED, never silently truncates the text),
 *   * any other raw control character (< 0x20) inside a string,
 *   * ill-formed UTF-8 (the strict reader validates UTF-8; the ALLOW_INVALID_-
 *     UNICODE flag is deliberately NOT set),
 *   * a duplicate object key (detected after the key sort, by adjacent-equal
 *     bytes -- see the rule above),
 *   * nesting deeper than the emitter's bound (k_canonical_max_depth, ~512): a
 *     deeply-nested but otherwise valid document is refused rather than risking
 *     the caller's stack; this is an input limit, not memory corruption.
 * KIMIX_ERR_INVALID_ARG is a programming error at the boundary: NULL `out`, or
 * NULL `utf8` with len > 0.  KIMIX_ERR_INVALID_STATE means `out` already holds a
 * live vector (constructing into it would leak that buffer).  Nothing returns
 * KIMIX_ERR_OUT_OF_MEMORY: the result vector's allocator aborts on exhaustion
 * (see api/ffi_vec.h), so a huge canonical document crashes rather than fails.
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.
 */
#pragma once
#ifndef KIMIX_API_FFI_CANONICAL_H
#define KIMIX_API_FFI_CANONICAL_H

#include <api/ffi_common.h>
#include <api/ffi_vec.h>

KIMIX_FFI_BEGIN

/* ===========================================================================
 * Canonical JSON into a caller-owned placeholder
 * ======================================================================== */

/* Re-emit the JSON document in [utf8, utf8 + len) in the canonical byte form
 * described at the top of this header, constructing the result as a new
 * `kimix::vector<std::byte>` in `out` (which must be RAW storage -- not yet
 * initialised, or already destroyed -- exactly like kimix_repair).
 *
 * `utf8` is KIMIX_IN input read during the call, in BYTES, NUL-termination NOT
 * required and NOT assumed: an embedded NUL is part of the text and is rejected
 * by the strict parser as a control character (see the input-acceptance table).
 * On success `out` holds the canonical bytes with NO trailing NUL
 * (kimix_vec_size() == the canonical length).
 *
 * Returns:
 *   KIMIX_OK                result constructed in `out`;
 *   KIMIX_ERR_INVALID_ARG   NULL `out`, or NULL `utf8` with len > 0;
 *   KIMIX_ERR_INVALID_STATE `out` already holds a live vector;
 *   KIMIX_ERR_INVALID_INPUT the bytes are not an acceptable canonical input
 *                           (malformed / empty / raw NUL / bad UTF-8 /
 *                           duplicate key / too deep).
 * On any non-OK return `out` is left exactly as it was. */
KIMIX_FFI kimix_status kimix_canonical_json(KIMIX_IN const char *utf8,
                                            size_t len,
                                            KIMIX_OUT kimix_vec *out);

/* The NUL-terminated spelling of kimix_canonical_json(): the input length is
 * strlen(nul_terminated).  Because the length is taken from the terminator, an
 * embedded NUL cannot be represented here -- it simply ends the input (and the
 * document it truncates is then rejected as malformed).  A NULL string is
 * KIMIX_ERR_INVALID_ARG (there is no length to fall back on).  Same canonical
 * form, same ownership, same error codes otherwise. */
KIMIX_FFI kimix_status kimix_canonical_json_str(KIMIX_IN const char *nul_terminated,
                                                KIMIX_OUT kimix_vec *out);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_CANONICAL_H */
