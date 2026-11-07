/* The API token and the lockout after wrong tokens. */
#include "esl_api_int.h"

bool esl_api_auth_ok(const char *token, const char *auth)
{
    if (!token || !*token || !auth)
        return false;
    static const char prefix[] = "bearer ";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        char c = auth[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != prefix[i])
            return false;
    }
    const char *given = auth + sizeof(prefix) - 1;
    while (*given == ' ')
        given++;
    size_t tl = strlen(token), gl = strlen(given);
    /* Constant time with respect to the token contents. */
    unsigned diff = (unsigned)(tl ^ gl);
    for (size_t i = 0; i < gl; i++)
        diff |= (unsigned char)given[i] ^ (unsigned char)token[i % tl];
    return diff == 0;
}

unsigned esl_api_lock_wait(const esl_api_lock_t *lock, uint64_t now_ms)
{
    if (!lock || now_ms >= lock->locked_until_ms)
        return 0;
    return (unsigned)((lock->locked_until_ms - now_ms + 999) / 1000);
}

void esl_api_lock_note(esl_api_lock_t *lock, bool ok, uint64_t now_ms)
{
    if (!lock)
        return;
    if (ok) {
        lock->fails = 0;
    } else if (++lock->fails >= ESL_API_AUTH_TRIES) {
        lock->fails = 0;
        lock->locked_until_ms = now_ms + ESL_API_AUTH_LOCK_S * 1000u;
    }
}

int esl_api_auth_check(const esl_api_t *api, const char *auth, uint64_t now_ms, unsigned *wait_s)
{
    *wait_s = esl_api_lock_wait(api->lock, now_ms);
    if (*wait_s)
        return 429;
    bool ok = esl_api_auth_ok(api->token, auth);
    esl_api_lock_note(api->lock, ok, now_ms);
    return ok ? 0 : 401;
}
