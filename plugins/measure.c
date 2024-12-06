#include "system.h"
#include "time.h"

#include <rpm/rpmlog.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmts.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmstring.h>

#include "rpmplugin.h"

struct measurestat {
    /* We're counting psm not packages because packages often run psm_pre/post
       more than once and we want to accumulate the time
    */
    unsigned int psm_count;
    unsigned int scriptlet_count;
    struct timespec plugin_start;
    struct timespec psm_start;
    struct timespec scriptlet_start;
};

static rpmRC push(const char *format, const char *value, const char *formatted)
{
    char *key = NULL;
    rpmRC rc = RPMRC_FAIL;
    if (formatted == NULL) {
        /* yes we're okay with discarding const here */
        key = (char *)format;
    } else {
        if (rasprintf(&key, format, formatted) == -1) {
            rpmlog(
                RPMLOG_ERR,
                _("measure: Failed to allocate formatted key %s, %s\n"),
                format,
                formatted
            );
            goto exit;
        }
    }
    if (rpmPushMacro(NULL, key, NULL, value, RMIL_GLOBAL)) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to set %s\n"), key);
        goto exit;
    }
    rc = RPMRC_OK;
exit:
    if (formatted != NULL) {
        free(key);
    }
    return rc;
}

static rpmRC diff_ms(char **ms, struct timespec *start, struct timespec *end)
{
    if (rasprintf(ms, "%ld", (
        (end->tv_sec - start->tv_sec) * 1000 +
        (end->tv_nsec - start->tv_nsec) / 1000000
    )) == -1) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to allocate formatted ms\n"));
        return RPMRC_FAIL;
    }
    return RPMRC_OK;
}

static rpmRC measure_init(rpmPlugin plugin, rpmts ts)
{
    rpmRC rc = RPMRC_FAIL;
    struct measurestat *state = rcalloc(1, sizeof(*state));
    if (clock_gettime(CLOCK_MONOTONIC, &state->plugin_start)) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to get plugin_start time\n"));
        goto exit;
    }
    state->psm_count = 0;
    state->scriptlet_count = 0;
    rpmPluginSetData(plugin, state);
    rc = RPMRC_OK;
exit:
    return rc;
}

static void measure_cleanup(rpmPlugin plugin)
{
    struct measurestat *state = rpmPluginGetData(plugin);
    free(state);
}

static rpmRC measure_tsm_post(rpmPlugin plugin, rpmts ts, int res)
{
    struct measurestat *state = rpmPluginGetData(plugin);
    char *psm_count = NULL, *scriptlet_count = NULL;
    rpmRC rc = RPMRC_FAIL;

    if (rasprintf(&psm_count, "%d", state->psm_count) == -1) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to allocate formatted psm_count\n"));
        goto exit;
    }
    if (rasprintf(&scriptlet_count, "%d", state->scriptlet_count) == -1) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to allocate formatted scriptlet_count\n"));
        goto exit;
    }
    if (push("_measure_plugin_psm_count", psm_count, NULL) != RPMRC_OK) {
        goto exit;
    }
    if (push("_measure_plugin_scriptlet_count", scriptlet_count, NULL) != RPMRC_OK) {
        goto exit;
    }
    rc = RPMRC_OK;
exit:
    free(psm_count);
    free(scriptlet_count);
    return rc;
}

static rpmRC measure_psm_pre(rpmPlugin plugin, rpmte te)
{
    struct measurestat *state = rpmPluginGetData(plugin);
    rpmRC rc = RPMRC_FAIL;

    if (clock_gettime(CLOCK_MONOTONIC, &state->psm_start)) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to get psm_start time\n"));
        goto exit;
    }
    rc = RPMRC_OK;
exit:
    return rc;
}

static rpmRC measure_psm_post(rpmPlugin plugin, rpmte te, int res)
{
    struct measurestat *state = rpmPluginGetData(plugin);
    struct timespec end;
    char *offset = NULL, *duration = NULL, *prefix = NULL;
    Header h = rpmteHeader(te);
    rpmRC rc = RPMRC_FAIL;

    if (clock_gettime(CLOCK_MONOTONIC, &end)) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to get psm end time\n"));
        goto exit;
    }
    if (rasprintf(&prefix, "_measure_plugin_package_%u", state->psm_count) == -1) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to allocate prefix\n"));
        goto exit;
    }
    if (diff_ms(&offset, &state->plugin_start, &state->psm_start) != RPMRC_OK) {
        goto exit;
    }
    if (diff_ms(&duration, &state->psm_start, &end) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_nevra", rpmteNEVRA(te), prefix) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_compressor", headerGetString(h, RPMTAG_PAYLOADCOMPRESSOR), prefix) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_offset", offset, prefix) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_ms", duration, prefix) != RPMRC_OK) {
        goto exit;
    }
    state->psm_count += 1;
    rc = RPMRC_OK;
exit:
    headerFree(h);
    free(prefix);
    free(duration);
    free(offset);
    return rc;
}

static rpmRC measure_scriptlet_pre(rpmPlugin plugin,
					const char *s_name, int type)
{
    struct measurestat *state = rpmPluginGetData(plugin);
    if (clock_gettime(CLOCK_MONOTONIC, &state->scriptlet_start)) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to get scriptlet_start time\n"));
        return RPMRC_FAIL;
    }
    return RPMRC_OK;
}

static rpmRC measure_scriptlet_post(rpmPlugin plugin,
					const char *s_name, int type, int res)
{
    struct measurestat *state = rpmPluginGetData(plugin);
    struct timespec end;
    char *offset = NULL, *duration = NULL, *prefix = NULL;
    rpmRC rc = RPMRC_FAIL;

    if (clock_gettime(CLOCK_MONOTONIC, &end)) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to get end time\n"));
        goto exit;
    }

    if (rasprintf(&prefix, "_measure_plugin_scriptlet_%d", state->scriptlet_count) == -1) {
        rpmlog(RPMLOG_ERR, _("measure: Failed to allocate formatted prefix\n"));
        goto exit;
    }
    if (diff_ms(&offset, &state->plugin_start, &state->scriptlet_start) != RPMRC_OK) {
        goto exit;
    }
    if (diff_ms(&duration, &state->scriptlet_start, &end) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_name", s_name, prefix) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_offset", offset, prefix) != RPMRC_OK) {
        goto exit;
    }
    if (push("%s_ms", duration, prefix) != RPMRC_OK) {
        goto exit;
    }
    state->scriptlet_count += 1;
    rc = RPMRC_OK;
exit:
    free(prefix);
    free(duration);
    free(offset);
    return rc;
}

struct rpmPluginHooks_s measure_hooks = {
    .init = measure_init,
    .cleanup = measure_cleanup,
    .tsm_post = measure_tsm_post,
    .psm_pre = measure_psm_pre,
    .psm_post = measure_psm_post,
    .scriptlet_pre = measure_scriptlet_pre,
    .scriptlet_post = measure_scriptlet_post,
};
