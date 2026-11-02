#pragma once
/* The AP's own HTML page from C (/setup and its answers) with the dashboard's look:
 * the same header, colours, font and cards. When the dashboard is in flash the
 * page also links its style.css (dark mode, and any later change to the colours);
 * the small style in the page itself uses the same variables with the light
 * colours as fallback, so a build without the dashboard looks the same. */

#include "esl_json.h"

/* Which pages this AP has, for the links in the header */
#define ESL_PAGE_DASHBOARD 0x1u /* /dashboard (and /ui/css/style.css) */
#define ESL_PAGE_SETUP 0x2u     /* /setup */

/* Everything up to the page's own content: the head, the header with the links in
   pages (the one at current marked as the page shown) and the opening <main>.
   title is plain ASCII. */
void esl_page_head(esl_sb_t *out, const char *title, unsigned pages, unsigned current);

/* Closes what esl_page_head opened */
void esl_page_tail(esl_sb_t *out);
