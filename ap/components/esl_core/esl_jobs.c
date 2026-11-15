/* Jobs: what became of each tag of a PUT /api/tags batch (esl_jobs.h). */
#include "esl_jobs.h"
#include <stdlib.h>
#include <string.h>

static void lock(esl_jobs_t *j)
{
    if (j->lock)
        j->lock(j->ctx);
}

static void unlock(esl_jobs_t *j)
{
    if (j->unlock)
        j->unlock(j->ctx);
}

void esl_jobs_init(esl_jobs_t *jobs, uint32_t first_id, unsigned max_tags)
{
    memset(jobs, 0, sizeof(*jobs));
    jobs->next_id = first_id ? first_id : 1;
    jobs->max_tags = max_tags < ESL_JOB_TAGS_MAX ? ESL_JOB_TAGS_MAX : max_tags;
}

static void forget(esl_jobs_t *jobs, esl_job_t *j)
{
    jobs->tags -= j->n;
    free(j->tags);
    memset(j, 0, sizeof(*j));
}

void esl_jobs_free(esl_jobs_t *jobs)
{
    for (unsigned i = 0; i < ESL_JOBS_MAX; i++)
        if (jobs->job[i].id)
            forget(jobs, &jobs->job[i]);
}

/* The oldest kept job, NULL when none. Numbers grow by one per job (and wrap), so the
   oldest is the one furthest behind next_id. */
static esl_job_t *oldest(esl_jobs_t *jobs)
{
    esl_job_t *o = NULL;
    for (unsigned i = 0; i < ESL_JOBS_MAX; i++) {
        esl_job_t *j = &jobs->job[i];
        if (j->id && (!o || jobs->next_id - j->id > jobs->next_id - o->id))
            o = j;
    }
    return o;
}

static esl_job_t *find(esl_jobs_t *jobs, uint32_t id)
{
    for (unsigned i = 0; id && i < ESL_JOBS_MAX; i++)
        if (jobs->job[i].id == id)
            return &jobs->job[i];
    return NULL;
}

uint32_t esl_jobs_add(esl_jobs_t *jobs, unsigned n, uint32_t timeout_s, bool notify, uint64_t now_ms)
{
    if (n > ESL_JOB_TAGS_MAX)
        return 0;
    esl_job_tag_t *tags = calloc(n ? n : 1, sizeof(*tags));
    if (!tags)
        return 0;
    lock(jobs);
    esl_job_t *free_place = NULL;
    for (;;) {
        free_place = NULL;
        for (unsigned i = 0; i < ESL_JOBS_MAX && !free_place; i++)
            if (!jobs->job[i].id)
                free_place = &jobs->job[i];
        if (free_place && jobs->tags + n <= jobs->max_tags)
            break;
        forget(jobs, oldest(jobs)); /* there is one: the table is full, or holds tags */
    }
    esl_job_t *j = free_place;
    j->id = jobs->next_id++;
    if (!jobs->next_id)
        jobs->next_id = 1; /* 0 is no job */
    j->timeout_s = timeout_s;
    j->start_ms = now_ms;
    j->end_ms = n ? 0 : (now_ms ? now_ms : 1);
    j->notify = notify;
    j->n = (uint8_t)n;
    j->open = (uint8_t)n;
    j->tags = tags;
    jobs->tags += n;
    uint32_t id = j->id;
    unlock(jobs);
    return id;
}

static void close_tag(esl_job_t *j, esl_job_tag_t *t, uint8_t state, uint8_t why, uint64_t now_ms)
{
    if (t->state != ESL_JOB_ACCEPTED)
        return;
    t->state = state;
    t->why = why;
    if (state != ESL_JOB_ACCEPTED && --j->open == 0)
        j->end_ms = now_ms ? now_ms : 1;
}

void esl_jobs_set(esl_jobs_t *jobs, uint32_t id, unsigned k, uint64_t mac, uint32_t content_id, uint8_t state,
                  uint8_t why, uint64_t now_ms)
{
    lock(jobs);
    esl_job_t *j = find(jobs, id);
    if (j && k < j->n) {
        j->tags[k].mac = mac;
        j->tags[k].content_id = content_id;
        close_tag(j, &j->tags[k], state, why, now_ms);
    }
    unlock(jobs);
}

/* What the store says about one open tag: ESL_JOB_ACCEPTED while it says nothing new */
static uint8_t look(esl_store_t *store, const esl_job_tag_t *jt, uint8_t *why)
{
    esl_tag_t t;
    *why = ESL_JOB_WHY_NONE;
    if (!esl_store_get(store, jt->mac, &t) || !t.has_content || t.content_id != jt->content_id)
        return ESL_JOB_REPLACED;
    if (t.seen && t.image_id && t.shown_content_id == t.image_id)
        return ESL_JOB_DELIVERED;
    if (t.image_id && t.image_failed == t.image_id) {
        *why = ESL_JOB_WHY_DRAW_FAILED;
        return ESL_JOB_FAILED;
    }
    /* set when the content is stored, so it is about this content (esl_store.c) */
    uint8_t err = esl_tag_content_err(&t);
    if (err) {
        *why = (uint8_t)(ESL_JOB_WHY_CONTENT + err);
        return ESL_JOB_FAILED;
    }
    return ESL_JOB_ACCEPTED;
}

static void update(esl_jobs_t *jobs, esl_store_t *store, uint64_t now_ms)
{
    for (unsigned i = 0; i < ESL_JOBS_MAX; i++) {
        esl_job_t *j = &jobs->job[i];
        if (!j->id || !j->open)
            continue;
        bool late = now_ms >= j->start_ms && now_ms - j->start_ms >= (uint64_t)j->timeout_s * 1000;
        for (unsigned k = 0; k < j->n && j->open; k++) {
            esl_job_tag_t *t = &j->tags[k];
            if (t->state != ESL_JOB_ACCEPTED || !t->content_id)
                continue;
            uint8_t why, state = look(store, t, &why);
            if (state == ESL_JOB_ACCEPTED && late)
                state = ESL_JOB_TIMED_OUT;
            close_tag(j, t, state, why, now_ms);
        }
    }
}

void esl_jobs_update(esl_jobs_t *jobs, esl_store_t *store, uint64_t now_ms)
{
    lock(jobs);
    update(jobs, store, now_ms);
    unlock(jobs);
}

static void copy(const esl_job_t *from, esl_job_t *job, esl_job_tag_t *tags)
{
    *job = *from;
    memcpy(tags, from->tags, sizeof(*tags) * from->n);
    job->tags = tags;
}

bool esl_jobs_get(esl_jobs_t *jobs, uint32_t id, esl_job_t *job, esl_job_tag_t *tags)
{
    lock(jobs);
    esl_job_t *j = find(jobs, id);
    if (j)
        copy(j, job, tags);
    unlock(jobs);
    return j != NULL;
}

unsigned esl_jobs_list(esl_jobs_t *jobs, uint32_t *ids)
{
    unsigned n = 0;
    lock(jobs);
    for (unsigned i = 0; i < ESL_JOBS_MAX; i++)
        if (jobs->job[i].id)
            ids[n++] = jobs->job[i].id;
    /* oldest first: by distance behind next_id, largest first (a few, so insertion sort) */
    for (unsigned a = 1; a < n; a++) {
        uint32_t v = ids[a];
        unsigned b = a;
        for (; b > 0 && jobs->next_id - ids[b - 1] < jobs->next_id - v; b--)
            ids[b] = ids[b - 1];
        ids[b] = v;
    }
    unlock(jobs);
    return n;
}

bool esl_jobs_to_notify(esl_jobs_t *jobs, esl_store_t *store, uint64_t now_ms, esl_job_t *job, esl_job_tag_t *tags)
{
    bool found = false;
    lock(jobs);
    update(jobs, store, now_ms);
    for (unsigned i = 0; i < ESL_JOBS_MAX && !found; i++) {
        esl_job_t *j = &jobs->job[i];
        if (j->id && j->notify && !j->notified && !j->open) {
            copy(j, job, tags);
            found = true;
        }
    }
    unlock(jobs);
    return found;
}

void esl_jobs_notified(esl_jobs_t *jobs, uint32_t id, bool ok)
{
    lock(jobs);
    esl_job_t *j = find(jobs, id);
    if (j && (ok || ++j->tries >= ESL_JOB_NOTIFY_TRIES))
        j->notified = true;
    unlock(jobs);
}

const char *esl_job_state_name(uint8_t state)
{
    static const char *const names[ESL_JOB_STATES] = {"accepted", "delivered", "failed",
                                                     "timed_out", "replaced", "scheduled"};
    return state < ESL_JOB_STATES ? names[state] : "unknown";
}

const char *esl_job_why_text(uint8_t why)
{
    switch (why) {
    case ESL_JOB_WHY_TABLE_FULL:  return "tag table full";
    case ESL_JOB_WHY_NVS_FULL:    return "no room to save it (NVS full)";
    case ESL_JOB_WHY_SCHED_FULL:  return "schedule full";
    case ESL_JOB_WHY_DRAW_FAILED: return "the tag could not show it (drawing failed)";
    default:
        if (why > ESL_JOB_WHY_CONTENT) {
            const char *t = esl_content_err_text((uint8_t)(why - ESL_JOB_WHY_CONTENT));
            if (t)
                return t;
        }
        return "failed";
    }
}
