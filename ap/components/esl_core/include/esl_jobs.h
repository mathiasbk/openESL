#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_store.h"

/* Jobs (#421, docs/protocol.md "Jobs"): each PUT /api/tags batch gets a number, and
 * GET /api/jobs/{n} (and, if asked, the webhook) says per tag whether it got its new
 * content. Kept in RAM only, for the last ESL_JOBS_MAX batches and at most max_tags
 * tags between them (esl_jobs_init): a new job pushes out the oldest, done or not. A
 * restart forgets them; the numbers of a new boot do not follow on from the old ones. */

#define ESL_JOBS_MAX 16            /* jobs kept */
#define ESL_JOB_TAGS_MAX 50        /* tags in one job: ESL_API_MAX_BATCH */
#define ESL_JOB_TIMEOUT_S 3600     /* how long a tag may take, when the batch does not say */
#define ESL_JOB_TIMEOUT_MIN_S 10
#define ESL_JOB_TIMEOUT_MAX_S 604800 /* a week */
#define ESL_JOB_NOTIFY_TRIES 5     /* webhook posts of a finished job before it is given up */

/* What became of one tag's content. Only ESL_JOB_ACCEPTED changes later. */
typedef enum {
    ESL_JOB_ACCEPTED,  /* stored; the tag does not show it yet */
    ESL_JOB_DELIVERED, /* the tag reported the image of this content as shown */
    ESL_JOB_FAILED,    /* not stored, or the tag cannot show it (why) */
    ESL_JOB_TIMED_OUT, /* not shown within the job's timeout */
    ESL_JOB_REPLACED,  /* the tag got other content (or was deleted) before it showed this */
    ESL_JOB_SCHEDULED, /* it had "at": it goes to the schedule, and is not followed */
    ESL_JOB_STATES
} esl_job_state_t;

/* Why a tag failed (esl_job_tag_t.why) */
enum {
    ESL_JOB_WHY_NONE,
    ESL_JOB_WHY_TABLE_FULL,  /* not stored: the tag table is full */
    ESL_JOB_WHY_NVS_FULL,    /* not stored: no room to save it */
    ESL_JOB_WHY_SCHED_FULL,  /* not scheduled: the schedule is full */
    ESL_JOB_WHY_DRAW_FAILED, /* the tag could not show the image of it */
    ESL_JOB_WHY_CONTENT = 0x10 /* + ESL_CERR_*: why the content cannot be shown (esl_content_err_text) */
};

typedef struct {
    uint64_t mac;
    uint32_t content_id; /* the content it was given (0 for SCHEDULED and a failed store) */
    uint8_t state;       /* esl_job_state_t */
    uint8_t why;         /* ESL_JOB_WHY_*, for ESL_JOB_FAILED */
} esl_job_tag_t;

typedef struct {
    uint32_t id;         /* 0 = this place is free */
    uint32_t timeout_s;
    uint64_t start_ms;   /* when it was accepted (ms since boot) */
    uint64_t end_ms;     /* when its last tag left ESL_JOB_ACCEPTED, 0 while one has not */
    bool notify;         /* post it to the webhook when it ends */
    bool notified;       /* ... done, or given up after ESL_JOB_NOTIFY_TRIES */
    uint8_t tries;
    uint8_t n;
    uint8_t open;        /* tags still ESL_JOB_ACCEPTED */
    esl_job_tag_t *tags; /* n of them, from the heap */
} esl_job_t;

typedef struct {
    esl_job_t job[ESL_JOBS_MAX]; /* in no order; the oldest has the smallest start */
    unsigned tags;               /* tags in all jobs */
    unsigned max_tags;
    uint32_t next_id;
    /* Kept apart from the API's mutex, as the webhook's task reads them too. NULL: no lock */
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} esl_jobs_t;

/* first_id: the number of the first job (from a random number at boot, so a client's number
   from before a restart is not taken for a new job; 0 counts as 1). max_tags: at most this many
   tags in all kept jobs together (at least ESL_JOB_TAGS_MAX). The lock may be set after. */
void esl_jobs_init(esl_jobs_t *jobs, uint32_t first_id, unsigned max_tags);
void esl_jobs_free(esl_jobs_t *jobs);

/* A new job of n tags (all ESL_JOB_ACCEPTED with content_id 0; esl_jobs_set fills them in),
   making room by forgetting the oldest jobs. Its number, 0 when out of memory (no job). */
uint32_t esl_jobs_add(esl_jobs_t *jobs, unsigned n, uint32_t timeout_s, bool notify, uint64_t now_ms);
/* What became of tag k of job id when the batch was stored: state ESL_JOB_ACCEPTED with the
   content_id it was given, or a final state */
void esl_jobs_set(esl_jobs_t *jobs, uint32_t id, unsigned k, uint64_t mac, uint32_t content_id, uint8_t state,
                  uint8_t why, uint64_t now_ms);

/* Looks at the tags of every open job in the store: delivered, failed, replaced or timed out. */
void esl_jobs_update(esl_jobs_t *jobs, esl_store_t *store, uint64_t now_ms);

/* A copy of job id (tags into tags, room for ESL_JOB_TAGS_MAX) after esl_jobs_update; false
   when it is not kept. */
bool esl_jobs_get(esl_jobs_t *jobs, uint32_t id, esl_job_t *job, esl_job_tag_t *tags);
/* The ids of the kept jobs, oldest first, into ids (room for ESL_JOBS_MAX): how many */
unsigned esl_jobs_list(esl_jobs_t *jobs, uint32_t *ids);

/* For the webhook: a copy (as esl_jobs_get) of a job that ended and asked to be posted
   and is not yet; false when there is none. esl_jobs_notified() then says how it went. */
bool esl_jobs_to_notify(esl_jobs_t *jobs, esl_store_t *store, uint64_t now_ms, esl_job_t *job, esl_job_tag_t *tags);
void esl_jobs_notified(esl_jobs_t *jobs, uint32_t id, bool ok);

/* "accepted", "delivered", ... */
const char *esl_job_state_name(uint8_t state);
/* The text for a failed tag's why */
const char *esl_job_why_text(uint8_t why);
