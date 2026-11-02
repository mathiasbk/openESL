#include "esl_page.h"

/* The rules these pages need from web/css/style.css, with its light colours and its
   scale (font sizes, spacing, corners) as the fallback of each variable. Scoped to
   .ap-page, after the link, so they win over the dashboard's own rules where the
   two differ (labels, for one). */
static const char STYLE[] =
    "body.ap-page{margin:0;background:var(--bg,#f6f6f3);color:var(--text,#1d1d1b);"
    "font:var(--fs-md,15px)/1.45 system-ui,-apple-system,\"Segoe UI\",sans-serif}"
    ".ap-page *{box-sizing:border-box}"
    ".ap-page h1{font-size:var(--fs-xl,22px);margin:0}"
    ".ap-page h2{font-size:var(--fs-lg,17px);margin:0 0 var(--space-2,8px)}"
    ".ap-page a{color:var(--accent,#1f5fbf)}"
    ".ap-page .top{display:flex;align-items:center;gap:var(--space-5,24px);flex-wrap:wrap;"
    "padding:10px var(--space-4,16px);border-bottom:1px solid var(--line,#d9d9d3);background:var(--panel,#fff)}"
    ".ap-page .tabs{display:flex;flex-wrap:wrap;gap:var(--space-1,4px)}"
    ".ap-page .tabs a{padding:6px var(--space-3,12px);border-radius:var(--radius-sm,6px);color:inherit;"
    "text-decoration:none}"
    ".ap-page .tabs a[aria-current]{background:var(--accent,#1f5fbf);color:var(--accent-text,#fff)}"
    ".ap-page main{padding:var(--space-4,16px);max-width:1400px;margin:0 auto}"
    ".ap-page .card{background:var(--panel,#fff);border:1px solid var(--line,#d9d9d3);border-radius:var(--radius,8px);"
    "padding:var(--space-4,16px);margin-bottom:var(--space-4,16px);max-width:860px}"
    ".ap-page .card p{margin:0 0 var(--space-2,8px)}"
    ".ap-page small,.ap-page .muted{color:var(--muted,#6b6b66);font-size:var(--fs-sm,14px)}"
    ".ap-page .m,.ap-page .e{padding:10px var(--space-3,12px);border:1px solid var(--accent,#1f5fbf);"
    "border-left-width:4px;border-radius:var(--radius,8px)}"
    ".ap-page .e{border-color:var(--error,#b3261e);color:var(--error,#b3261e);font-weight:600}"
    ".ap-page form label{display:block;margin-top:var(--space-3,12px)}"
    ".ap-page input,.ap-page select,.ap-page button{font:inherit;color:var(--text,#1d1d1b);"
    "background:var(--panel,#fff);border:1px solid var(--line,#d9d9d3);border-radius:var(--radius-sm,6px);"
    "padding:6px 10px}"
    ".ap-page input,.ap-page select{display:block;width:100%;margin-top:var(--space-1,4px)}"
    ".ap-page button{margin-top:var(--space-4,16px);cursor:pointer;background:var(--accent,#1f5fbf);"
    "color:var(--accent-text,#fff);border-color:var(--accent,#1f5fbf)}"
    ".ap-page .card form+p{margin-top:var(--space-4,16px)}";

static void link(esl_sb_t *out, const char *href, const char *text, bool here)
{
    esl_sb_printf(out, "<a href=\"%s\"%s>%s</a>", href, here ? " aria-current=\"page\"" : "", text);
}

void esl_page_head(esl_sb_t *out, const char *title, unsigned pages, unsigned current)
{
    esl_sb_puts(out, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    esl_sb_printf(out, "<title>%s - OpenESL AP</title>", title);
    if (pages & ESL_PAGE_DASHBOARD)
        /* and the light or dark choice made on the dashboard (web/js/theme.js), which only
           its style.css has the dark colours for */
        esl_sb_puts(out, "<link rel=\"stylesheet\" href=\"/ui/css/style.css\"><script>try{var t=localStorage"
                         ".getItem(\"openesl.theme\");if(t==\"light\"||t==\"dark\")"
                         "document.documentElement.dataset.theme=t}catch(e){}</script>");
    esl_sb_puts(out, "<style>");
    esl_sb_puts(out, STYLE);
    esl_sb_puts(out, "</style></head><body class=\"ap-page\"><header class=\"top\"><h1>OpenESL</h1>"
                     "<nav class=\"tabs\">");
    if (pages & ESL_PAGE_DASHBOARD)
        link(out, "/dashboard", "Dashboard", current == ESL_PAGE_DASHBOARD);
    if (pages & ESL_PAGE_SETUP)
        link(out, "/setup", "WiFi and token", current == ESL_PAGE_SETUP);
    esl_sb_puts(out, "</nav></header><main>");
}

void esl_page_tail(esl_sb_t *out)
{
    esl_sb_puts(out, "</main></body></html>");
}
