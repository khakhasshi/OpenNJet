
/*
 * !!! DO NOT EDIT DIRECTLY !!!
 * This file was automatically generated from the following template:
 *
 * src/subsys/njt_subsys_lua_regex.c.tt2
 */


/*
 * Copyright (C) Yichun Zhang (agentzh)
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#if (NJT_PCRE)

#include "njt_stream_lua_pcrefix.h"
#include "njt_stream_lua_script.h"
#include "njt_stream_lua_util.h"


#if (PCRE_MAJOR >= 6 || NJT_PCRE2)
#   define LUA_HAVE_PCRE_DFA 1
#else
#   define LUA_HAVE_PCRE_DFA 0
#endif


#if (NJT_PCRE2)
static pcre2_compile_context  *njt_regex_compile_context;
static pcre2_match_context    *njt_regex_match_context;
static pcre2_match_data       *njt_regex_match_data;
static njt_uint_t              njt_regex_match_data_size = 0;

#define PCRE2_VERSION_SIZE     64
static char                    njt_pcre2_version[PCRE2_VERSION_SIZE];
#endif


#define NJT_LUA_RE_MODE_DFA          (1<<1)
#define NJT_LUA_RE_MODE_JIT          (1<<2)
#define NJT_LUA_RE_NO_UTF8_CHECK     (1<<4)

#define NJT_LUA_RE_DFA_MODE_WORKSPACE_COUNT (100)

#define NJT_LUA_RE_MIN_JIT_STACK_SIZE 32 * 1024


typedef struct {
    njt_pool_t                   *pool;
    u_char                       *name_table;
    int                           name_count;
    int                           name_entry_size;

    int                           ncaptures;
    int                          *captures;

#if (NJT_PCRE2)
    pcre2_code                   *regex;
    /*
     * pcre2 doesn't use pcre_extra any more,
     * just for keeping same memory layout in the lua ffi cdef
     */
    void                         *regex_sd;
#else
    pcre                         *regex;
    pcre_extra                   *regex_sd;
#endif

    njt_stream_lua_complex_value_t          *replace;

    /* only for (stap) debugging, and may be an invalid pointer */
    const u_char                 *pattern;
} njt_stream_lua_regex_t;


typedef struct {
    njt_str_t     pattern;
    njt_pool_t   *pool;
    njt_int_t     options;

#if (NJT_PCRE2)
    pcre2_code   *regex;
#else
    pcre         *regex;
#endif
    int           captures;
    njt_str_t     err;
} njt_stream_lua_regex_compile_t;


typedef struct {
    njt_stream_lua_request_t            *request;

#if (NJT_PCRE2)
    pcre2_code              *regex;
#else
    pcre                    *regex;
    pcre_extra              *regex_sd;
#endif
    int                      ncaptures;
    int                     *captures;
    int                      captures_len;
    uint8_t                  flags;
} njt_stream_lua_regex_ctx_t;


static njt_int_t njt_stream_lua_regex_compile(
    njt_stream_lua_regex_compile_t *rc);


#define njt_stream_lua_regex_exec(re, e, s, start, captures, size,           \
                                  opts)                                      \
    pcre_exec(re, e, (const char *) (s)->data, (s)->len, start, opts,        \
              captures, size)


#define njt_stream_lua_regex_dfa_exec(re, e, s, start, captures, size,       \
                                      ws, wscount, opts)                     \
    pcre_dfa_exec(re, e, (const char *) (s)->data, (s)->len, start, opts,    \
                  captures, size, ws, wscount)


static void
njt_stream_lua_regex_free_study_data(njt_pool_t *pool,
    njt_stream_lua_regex_t *re)
{
    njt_pool_t  *old_pool;

#if (NJT_PCRE2)
    if (re && re->regex) {
        old_pool = njt_stream_lua_pcre_malloc_init(pool);

        pcre2_code_free(re->regex);

        njt_stream_lua_pcre_malloc_done(old_pool);

        re->regex = NULL;
    }
#else
    if (re && re->regex_sd) {
        old_pool = njt_stream_lua_pcre_malloc_init(pool);
#if LUA_HAVE_PCRE_JIT
        pcre_free_study(re->regex_sd);
#else
        pcre_free(re->regex_sd);
#endif
        njt_stream_lua_pcre_malloc_done(old_pool);

        re->regex_sd = NULL;
    }
#endif
}


#if (NJT_PCRE2)
static njt_int_t
njt_stream_lua_regex_compile(njt_stream_lua_regex_compile_t *rc)
{
    int                     n, errcode;
    char                   *p;
    size_t                  erroff;
    u_char                  errstr[128];
    pcre2_code             *re;
    njt_pool_t             *old_pool;
    pcre2_general_context  *gctx;
    pcre2_compile_context  *cctx;

    njt_stream_lua_main_conf_t  *lmcf;

    if (njt_regex_compile_context == NULL) {
        /*
         * Allocate a compile context if not yet allocated.  This uses
         * direct allocations from heap, so the result can be cached
         * even at runtime.
         */

        old_pool = njt_stream_lua_pcre_malloc_init(NULL);

        gctx = pcre2_general_context_create(njt_stream_lua_pcre_malloc,
                                            njt_stream_lua_pcre_free,
                                            NULL);
        if (gctx == NULL) {
            njt_stream_lua_pcre_malloc_done(old_pool);
            goto nomem;
        }

        cctx = pcre2_compile_context_create(gctx);
        if (cctx == NULL) {
            pcre2_general_context_free(gctx);
            njt_stream_lua_pcre_malloc_done(old_pool);
            goto nomem;
        }

        njt_regex_compile_context = cctx;

        njt_regex_match_context = pcre2_match_context_create(gctx);
        if (njt_regex_match_context == NULL) {
            pcre2_general_context_free(gctx);
            njt_stream_lua_pcre_malloc_done(old_pool);
            goto nomem;
        }

        lmcf = njt_stream_cycle_get_module_main_conf(njt_cycle,
                                                   njt_stream_lua_module);
        if (lmcf && lmcf->regex_match_limit > 0) {
            pcre2_set_match_limit(njt_regex_match_context,
                                  lmcf->regex_match_limit);
        }

        pcre2_general_context_free(gctx);
        njt_stream_lua_pcre_malloc_done(old_pool);
    }

    old_pool = njt_stream_lua_pcre_malloc_init(rc->pool);

    re = pcre2_compile(rc->pattern.data,
                       rc->pattern.len, rc->options,
                       &errcode, &erroff, njt_regex_compile_context);

    njt_stream_lua_pcre_malloc_done(old_pool);

    if (re == NULL) {
        pcre2_get_error_message(errcode, errstr, 128);

        if ((size_t) erroff == rc->pattern.len) {
            rc->err.len = njt_snprintf(rc->err.data, rc->err.len,
                                       "pcre2_compile() failed: %s in \"%V\"",
                                       errstr, &rc->pattern)
                          - rc->err.data;

        } else {
            rc->err.len = njt_snprintf(rc->err.data, rc->err.len,
                                       "pcre2_compile() failed: %s in "
                                       "\"%V\" at \"%s\"", errstr, &rc->pattern,
                                       rc->pattern.data + erroff)
                          - rc->err.data;
        }

        return NJT_ERROR;
    }

    rc->regex = re;

    n = pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &rc->captures);
    if (n < 0) {
        p = "pcre2_pattern_info(\"%V\", PCRE_INFO_CAPTURECOUNT) failed: %d";
        goto failed;
    }

#if (NJT_DEBUG)
    njt_log_debug3(NJT_LOG_DEBUG_STREAM, njt_cycle->log, 0,
                   "pcre2_compile: pattern[%V], options 0x%08Xd, ncaptures %d",
                   &rc->pattern, rc->options, rc->captures);
#endif

    return NJT_OK;

failed:

    rc->err.len = njt_snprintf(rc->err.data, rc->err.len, p, &rc->pattern, n)
                  - rc->err.data;
    return NJT_ERROR;

nomem:

    rc->err.len = njt_snprintf(rc->err.data, rc->err.len,
                               "regex \"%V\" compilation failed: no memory",
                               &rc->pattern)
                  - rc->err.data;
    return NJT_ERROR;
}

#else

static njt_int_t
njt_stream_lua_regex_compile(njt_stream_lua_regex_compile_t *rc)
{
    int           n, erroff;
    char         *p;
    const char   *errstr;
    pcre         *re;
    njt_pool_t   *old_pool;

    old_pool = njt_stream_lua_pcre_malloc_init(rc->pool);

    re = pcre_compile((const char *) rc->pattern.data, (int) rc->options,
                      &errstr, &erroff, NULL);

    njt_stream_lua_pcre_malloc_done(old_pool);

    if (re == NULL) {
        if ((size_t) erroff == rc->pattern.len) {
            rc->err.len = njt_snprintf(rc->err.data, rc->err.len,
                                       "pcre_compile() failed: %s in \"%V\"",
                                       errstr, &rc->pattern)
                         - rc->err.data;

        } else {
            rc->err.len = njt_snprintf(rc->err.data, rc->err.len,
                                       "pcre_compile() failed: %s in \"%V\" "
                                       "at \"%s\"", errstr, &rc->pattern,
                                       rc->pattern.data + erroff)
                         - rc->err.data;
        }

        return NJT_ERROR;
    }

    rc->regex = re;

#if 1
    n = pcre_fullinfo(re, NULL, PCRE_INFO_CAPTURECOUNT, &rc->captures);
    if (n < 0) {
        p = "pcre_fullinfo(\"%V\", PCRE_INFO_CAPTURECOUNT) failed: %d";
        goto failed;
    }
#endif

    return NJT_OK;

failed:

    rc->err.len = njt_snprintf(rc->err.data, rc->err.len, p, &rc->pattern, n)
                  - rc->err.data;
    return NJT_OK;
}
#endif


njt_int_t
njt_stream_lua_ffi_set_jit_stack_size(int size, u_char *errstr,
    size_t *errstr_size)
{
#if (LUA_HAVE_PCRE_JIT)

    njt_stream_lua_main_conf_t          *lmcf;
    njt_pool_t                          *pool, *old_pool;

    lmcf = njt_stream_cycle_get_module_main_conf(njt_cycle,
                                                 njt_stream_lua_module);

    if (size < NJT_LUA_RE_MIN_JIT_STACK_SIZE) {
        size = NJT_LUA_RE_MIN_JIT_STACK_SIZE;
    }

    pool = lmcf->pool;

    dd("server pool %p", lmcf->pool);

    if (lmcf->jit_stack) {
        old_pool = njt_stream_lua_pcre_malloc_init(pool);

#if (NJT_PCRE2)
        pcre2_jit_stack_free(lmcf->jit_stack);
#else
        pcre_jit_stack_free(lmcf->jit_stack);
#endif

        njt_stream_lua_pcre_malloc_done(old_pool);
    }

    old_pool = njt_stream_lua_pcre_malloc_init(pool);

#if (NJT_PCRE2)
    lmcf->jit_stack = pcre2_jit_stack_create(NJT_LUA_RE_MIN_JIT_STACK_SIZE,
                                             size, NULL);
#else
    lmcf->jit_stack = pcre_jit_stack_alloc(NJT_LUA_RE_MIN_JIT_STACK_SIZE,
                                           size);
#endif

    njt_stream_lua_pcre_malloc_done(old_pool);

    if (lmcf->jit_stack == NULL) {
        *errstr_size = njt_snprintf(errstr, *errstr_size,
                                    "pcre jit stack allocation failed")
                       - errstr;
        return NJT_ERROR;
    }

    return NJT_OK;

#else  /* LUA_HAVE_PCRE_JIT */

    *errstr_size = njt_snprintf(errstr, *errstr_size,
                                "no pcre jit support found") - errstr;
    return NJT_ERROR;

#endif
}


#if (NJT_PCRE2)
static void
njt_stream_lua_regex_jit_compile(njt_stream_lua_regex_t *re, int flags,
    njt_pool_t *pool, njt_stream_lua_main_conf_t *lmcf,
    njt_stream_lua_regex_compile_t *re_comp)
{
    njt_int_t    ret;
    njt_pool_t  *old_pool;

    if (flags & NJT_LUA_RE_MODE_JIT) {
        old_pool = njt_stream_lua_pcre_malloc_init(pool);
        ret = pcre2_jit_compile(re_comp->regex, PCRE2_JIT_COMPLETE);

        if (ret != 0) {
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                          "pcre2_jit_compile() failed: %d in \"%V\", "
                          "ignored",
                          ret, &re_comp->pattern);

#if (NJT_DEBUG)

        } else {
            njt_log_debug0(NJT_LOG_DEBUG_STREAM, njt_cycle->log, 0,
                           "pcre2 JIT compiled successfully");
#   endif /* !(NJT_DEBUG) */
        }

        njt_stream_lua_pcre_malloc_done(old_pool);

    }

    if (lmcf && lmcf->jit_stack) {
        pcre2_jit_stack_assign(njt_regex_match_context, NULL,
                               lmcf->jit_stack);
    }

    return;
}

#else

static void
njt_stream_lua_regex_jit_compile(njt_stream_lua_regex_t *re, int flags,
    njt_pool_t *pool, njt_stream_lua_main_conf_t *lmcf,
    njt_stream_lua_regex_compile_t *re_comp)
{
    const char  *msg;
    pcre_extra  *sd = NULL;
    njt_pool_t  *old_pool;


#if (LUA_HAVE_PCRE_JIT)
    if (flags & NJT_LUA_RE_MODE_JIT) {
        old_pool = njt_stream_lua_pcre_malloc_init(pool);
        sd = pcre_study(re_comp->regex, PCRE_STUDY_JIT_COMPILE, &msg);
        njt_stream_lua_pcre_malloc_done(old_pool);

#   if (NJT_DEBUG)
        if (msg != NULL) {
            njt_log_debug2(NJT_LOG_DEBUG_STREAM, njt_cycle->log, 0,
                           "pcre study failed with PCRE_STUDY_JIT_COMPILE: "
                           "%s (%p)", msg, sd);
        }

        if (sd != NULL) {
            int         jitted;

            old_pool = njt_stream_lua_pcre_malloc_init(pool);

            pcre_fullinfo(re_comp->regex, sd, PCRE_INFO_JIT, &jitted);

            njt_stream_lua_pcre_malloc_done(old_pool);

            njt_log_debug1(NJT_LOG_DEBUG_STREAM, njt_cycle->log, 0,
                           "pcre JIT compiling result: %d", jitted);
        }
#   endif /* !(NJT_DEBUG) */

    } else {
        old_pool = njt_stream_lua_pcre_malloc_init(pool);
        sd = pcre_study(re_comp->regex, 0, &msg);
        njt_stream_lua_pcre_malloc_done(old_pool);
    }

    if (sd && lmcf && lmcf->jit_stack) {
        pcre_assign_jit_stack(sd, NULL, lmcf->jit_stack);
    }

    if (sd
        && lmcf && lmcf->regex_match_limit > 0
        && !(flags & NJT_LUA_RE_MODE_DFA))
    {
        sd->flags |= PCRE_EXTRA_MATCH_LIMIT;
        sd->match_limit = lmcf->regex_match_limit;
    }

#endif /* LUA_HAVE_PCRE_JIT */

    re->regex_sd = sd;
}
#endif


#if (NJT_PCRE2)
void
njt_stream_lua_regex_cleanup(void *data)
{
    njt_pool_t                  *old_pool;
    njt_stream_lua_main_conf_t  *lmcf;

    lmcf = data;

    if (njt_regex_compile_context) {
        old_pool = njt_stream_lua_pcre_malloc_init(NULL);
        pcre2_compile_context_free(njt_regex_compile_context);
        njt_regex_compile_context = NULL;
        njt_stream_lua_pcre_malloc_done(old_pool);
    }

    if (lmcf && lmcf->jit_stack) {
        old_pool = njt_stream_lua_pcre_malloc_init(NULL);

        pcre2_jit_stack_free(lmcf->jit_stack);
        lmcf->jit_stack = NULL;

        njt_stream_lua_pcre_malloc_done(old_pool);
    }

    if (njt_regex_match_data) {
        old_pool = njt_stream_lua_pcre_malloc_init(NULL);
        pcre2_match_data_free(njt_regex_match_data);
        njt_regex_match_data = NULL;
        njt_regex_match_data_size = 0;
        njt_stream_lua_pcre_malloc_done(old_pool);
    }

}
#endif


njt_stream_lua_regex_t *
njt_stream_lua_ffi_compile_regex(const unsigned char *pat, size_t pat_len,
    int flags, int pcre_opts, u_char *errstr,
    size_t errstr_size)
{
    int                             *cap = NULL, ovecsize;
    u_char                          *p;
    njt_int_t                        rc;
    const char                      *msg;
    njt_pool_t                      *pool, *old_pool;
    njt_stream_lua_regex_t          *re = NULL;
    njt_stream_lua_main_conf_t      *lmcf;
    njt_stream_lua_regex_compile_t   re_comp;

    pool = njt_create_pool(512, njt_cycle->log);
    if (pool == NULL) {
        msg = "no memory";
        goto error;
    }

    pool->log = (njt_log_t *) &njt_cycle->new_log;

    re = njt_palloc(pool, sizeof(njt_stream_lua_regex_t));
    if (re == NULL) {
        njt_destroy_pool(pool);
        pool = NULL;
        msg = "no memory";
        goto error;
    }

    re->pool = pool;
    re->regex = NULL;
    re->regex_sd = NULL;

    re_comp.options      = pcre_opts;
    re_comp.pattern.data = (u_char *) pat;
    re_comp.pattern.len  = pat_len;
    re_comp.err.len      = errstr_size - 1;
    re_comp.err.data     = errstr;
    re_comp.pool         = pool;

    old_pool = njt_stream_lua_pcre_malloc_init(pool);
    rc = njt_stream_lua_regex_compile(&re_comp);
    njt_stream_lua_pcre_malloc_done(old_pool);

    if (rc != NJT_OK) {
        re_comp.err.data[re_comp.err.len] = '\0';
        msg = (char *) re_comp.err.data;
        goto error;
    }

    lmcf = njt_stream_cycle_get_module_main_conf(njt_cycle,
                                                 njt_stream_lua_module);

    njt_stream_lua_regex_jit_compile(re, flags, pool, lmcf, &re_comp);

    if (flags & NJT_LUA_RE_MODE_DFA) {
        ovecsize = 2;
        re_comp.captures = 0;

    } else {
        ovecsize = (re_comp.captures + 1) * 3;
    }

    dd("allocating cap with size: %d", (int) ovecsize);

    cap = njt_palloc(pool, ovecsize * sizeof(int));
    if (cap == NULL) {
        msg = "no memory";
        goto error;
    }

#if (NJT_PCRE2)
    if (pcre2_pattern_info(re_comp.regex, PCRE2_INFO_NAMECOUNT,
                           &re->name_count) < 0)
    {
        msg = "cannot acquire named subpattern count";
        goto error;
    }

    if (re->name_count > 0) {
        if (pcre2_pattern_info(re_comp.regex, PCRE2_INFO_NAMEENTRYSIZE,
                               &re->name_entry_size) != 0)
        {
            msg = "cannot acquire named subpattern entry size";
            goto error;
        }

        if (pcre2_pattern_info(re_comp.regex, PCRE2_INFO_NAMETABLE,
                               &re->name_table) != 0)
        {
            msg = "cannot acquire named subpattern table";
            goto error;
        }
    }

#else
    if (pcre_fullinfo(re_comp.regex, NULL, PCRE_INFO_NAMECOUNT,
                      &re->name_count) != 0)
    {
        msg = "cannot acquire named subpattern count";
        goto error;
    }

    if (re->name_count > 0) {
        if (pcre_fullinfo(re_comp.regex, NULL, PCRE_INFO_NAMEENTRYSIZE,
                          &re->name_entry_size) != 0)
        {
            msg = "cannot acquire named subpattern entry size";
            goto error;
        }

        if (pcre_fullinfo(re_comp.regex, NULL, PCRE_INFO_NAMETABLE,
                          &re->name_table) != 0)
        {
            msg = "cannot acquire named subpattern table";
            goto error;
        }
    }
#endif

    re->regex = re_comp.regex;
    re->ncaptures = re_comp.captures;
    re->captures = cap;
    re->replace = NULL;

    /* only for (stap) debugging, the pointer might be invalid when the
     * string is collected later on.... */
    re->pattern = pat;

    return re;

error:

    p = njt_snprintf(errstr, errstr_size - 1, "%s", msg);
    *p = '\0';

    njt_stream_lua_regex_free_study_data(pool, re);

    if (pool) {
        njt_destroy_pool(pool);
    }

    return NULL;
}


#if (NJT_PCRE2)
int
njt_stream_lua_ffi_exec_regex(njt_stream_lua_regex_t *re, int flags,
    const u_char *s, size_t len, int pos)
{
    int          rc, exec_opts = 0;
    size_t      *ov;
    njt_uint_t   ovecsize, n, i;
    njt_pool_t  *old_pool;

    if (flags & NJT_LUA_RE_MODE_DFA) {
        ovecsize = 2;
        re->ncaptures = 0;

    } else {
        ovecsize = (re->ncaptures + 1) * 3;
    }

    old_pool = njt_stream_lua_pcre_malloc_init(NULL);

    if (njt_regex_match_data == NULL
        || ovecsize > njt_regex_match_data_size)
    {
        /*
         * Allocate a match data if not yet allocated or smaller than
         * needed.
         */

        if (njt_regex_match_data) {
            pcre2_match_data_free(njt_regex_match_data);
        }

        njt_regex_match_data_size = ovecsize;
        njt_regex_match_data = pcre2_match_data_create(ovecsize / 3, NULL);

        if (njt_regex_match_data == NULL) {
            rc = PCRE2_ERROR_NOMEMORY;
            goto failed;
        }
    }

    if (flags & NJT_LUA_RE_NO_UTF8_CHECK) {
        exec_opts = PCRE2_NO_UTF_CHECK;

    } else {
        exec_opts = 0;
    }

    if (flags & NJT_LUA_RE_MODE_DFA) {
        int ws[NJT_LUA_RE_DFA_MODE_WORKSPACE_COUNT];
        rc = pcre2_dfa_match(re->regex, s, len, pos, exec_opts,
                             njt_regex_match_data, njt_regex_match_context,
                             ws, sizeof(ws) / sizeof(ws[0]));

    } else {
        rc = pcre2_match(re->regex, s, len, pos, exec_opts,
                         njt_regex_match_data, njt_regex_match_context);
    }

    if (rc < 0) {
#if (NJT_DEBUG)
    njt_log_debug4(NJT_LOG_DEBUG_STREAM, njt_cycle->log, 0,
                   "pcre2_match failed: flags 0x%05Xd, options 0x%08Xd, rc %d, "
                   "ovecsize %ui", flags, exec_opts, rc, ovecsize);
#endif

        goto failed;
    }

    n = pcre2_get_ovector_count(njt_regex_match_data);
    ov = pcre2_get_ovector_pointer(njt_regex_match_data);

#if (NJT_DEBUG)
    njt_log_debug5(NJT_LOG_DEBUG_STREAM, njt_cycle->log, 0,
                   "pcre2_match: flags 0x%05Xd, options 0x%08Xd, rc %d, "
                   "n %ui, ovecsize %ui", flags, exec_opts, rc, n, ovecsize);
#endif

    if (!(flags & NJT_LUA_RE_MODE_DFA) && n > ovecsize / 3) {
        n = ovecsize / 3;
    }

    for (i = 0; i < n; i++) {
        re->captures[i * 2] = ov[i * 2];
        re->captures[i * 2 + 1] = ov[i * 2 + 1];
    }

failed:

    njt_stream_lua_pcre_malloc_done(old_pool);

    return rc;
}

#else

int
njt_stream_lua_ffi_exec_regex(njt_stream_lua_regex_t *re, int flags,
    const u_char *s, size_t len, int pos)
{
    int             rc, ovecsize, exec_opts, *cap;
    njt_str_t       subj;
    pcre_extra     *sd;

    cap = re->captures;
    sd = re->regex_sd;

    if (flags & NJT_LUA_RE_MODE_DFA) {
        ovecsize = 2;
        re->ncaptures = 0;

    } else {
        ovecsize = (re->ncaptures + 1) * 3;
    }

    if (flags & NJT_LUA_RE_NO_UTF8_CHECK) {
        exec_opts = PCRE_NO_UTF8_CHECK;

    } else {
        exec_opts = 0;
    }

    subj.data = (u_char *) s;
    subj.len = len;

    if (flags & NJT_LUA_RE_MODE_DFA) {

#if LUA_HAVE_PCRE_DFA

        int ws[NJT_LUA_RE_DFA_MODE_WORKSPACE_COUNT];
        rc = njt_stream_lua_regex_dfa_exec(re->regex, sd, &subj,
                                           (int) pos, cap, ovecsize, ws,
                                           sizeof(ws) / sizeof(ws[0]), exec_opts);

#else

        return PCRE_ERROR_BADOPTION;

#endif /* LUA_HAVE_PCRE_DFA */

    } else {
        rc = njt_stream_lua_regex_exec(re->regex, sd, &subj, (int) pos, cap,
                                       ovecsize, exec_opts);
    }

    return rc;
}

#endif


void
njt_stream_lua_ffi_destroy_regex(njt_stream_lua_regex_t *re)
{
    dd("destroy regex called");

    if (re == NULL || re->pool == NULL) {
        return;
    }

    njt_stream_lua_regex_free_study_data(re->pool, re);

    njt_destroy_pool(re->pool);
}


int
njt_stream_lua_ffi_compile_replace_template(njt_stream_lua_regex_t *re,
    const u_char *replace_data, size_t replace_len)
{
    njt_int_t                                rc;
    njt_str_t                                tpl;

    njt_stream_lua_complex_value_t                  *ctpl;
    njt_stream_lua_compile_complex_value_t           ccv;

    ctpl = njt_palloc(re->pool, sizeof(njt_stream_lua_complex_value_t));
    if (ctpl == NULL) {
        return NJT_ERROR;
    }

    if (replace_len != 0) {
        /* copy the string buffer pointed to by tpl.data from Lua VM */
        tpl.data = njt_palloc(re->pool, replace_len + 1);
        if (tpl.data == NULL) {
            return NJT_ERROR;
        }

        njt_memcpy(tpl.data, replace_data, replace_len);
        tpl.data[replace_len] = '\0';

    } else {
        tpl.data = (u_char *) replace_data;
    }

    tpl.len = replace_len;

    njt_memzero(&ccv, sizeof(njt_stream_lua_compile_complex_value_t));
    ccv.pool = re->pool;
    ccv.log = njt_cycle->log;
    ccv.value = &tpl;
    ccv.complex_value = ctpl;

    rc = njt_stream_lua_compile_complex_value(&ccv);

    re->replace = ctpl;

    return rc;
}


njt_stream_lua_script_engine_t *
njt_stream_lua_ffi_create_script_engine(void)
{
    return njt_calloc(sizeof(njt_stream_lua_script_engine_t), njt_cycle->log);
}


void
njt_stream_lua_ffi_init_script_engine(njt_stream_lua_script_engine_t *e,
    const unsigned char *subj, njt_stream_lua_regex_t *compiled, int count)
{
    e->log = njt_cycle->log;
    e->ncaptures = count * 2;
    e->captures = compiled->captures;
    e->captures_data = (u_char *) subj;
}


void
njt_stream_lua_ffi_destroy_script_engine(njt_stream_lua_script_engine_t *e)
{
    njt_free(e);
}


size_t
njt_stream_lua_ffi_script_eval_len(njt_stream_lua_script_engine_t *e,
    njt_stream_lua_complex_value_t *val)
{
    size_t          len;

    njt_stream_lua_script_len_code_pt         lcode;

    e->ip = val->lengths;
    len = 0;

    while (*(uintptr_t *) e->ip) {
        lcode = *(njt_stream_lua_script_len_code_pt *) e->ip;
        len += lcode(e);
    }

    return len;
}


void
njt_stream_lua_ffi_script_eval_data(njt_stream_lua_script_engine_t *e,
    njt_stream_lua_complex_value_t *val, u_char *dst)
{
    njt_stream_lua_script_code_pt             code;

    e->ip = val->values;
    e->pos = dst;

    while (*(uintptr_t *) e->ip) {
        code = *(njt_stream_lua_script_code_pt *) e->ip;
        code(e);
    }
}


uint32_t
njt_stream_lua_ffi_max_regex_cache_size(void)
{
    njt_stream_lua_main_conf_t          *lmcf;
    lmcf = njt_stream_cycle_get_module_main_conf(njt_cycle,
                                                 njt_stream_lua_module);
    if (lmcf == NULL) {
        return 0;
    }
    return (uint32_t) lmcf->regex_cache_max_entries;
}


const char *
njt_stream_lua_ffi_pcre_version(void)
{
#if (NJT_PCRE2)
    pcre2_config(PCRE2_CONFIG_VERSION, njt_pcre2_version);

    return njt_pcre2_version;
#else
    return pcre_version();
#endif
}


#endif /* NJT_PCRE */


/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
