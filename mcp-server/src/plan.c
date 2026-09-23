/* plan.c - loading and querying the Joist design plan. */
#include "plan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int plan_load(plan_doc *doc, const char *path)
{
    doc->text = NULL;
    doc->len = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1U);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1U, (size_t)size, f);
    fclose(f);

    buf[got] = '\0';
    doc->text = buf;
    doc->len = got;
    return 0;
}

void plan_free(plan_doc *doc)
{
    free(doc->text);
    doc->text = NULL;
    doc->len = 0;
}

/* Number of leading '#' characters if line is a Markdown heading, else 0. */
static int heading_level(const char *line)
{
    int level = 0;
    while (line[level] == '#') {
        level++;
    }
    if (level > 0 && line[level] == ' ') {
        return level;
    }
    return 0;
}

/* Pointer just past the '#'s and the single space of a heading line. */
static const char *heading_title(const char *line, int level)
{
    return line + level + 1;
}

/* Case-insensitive substring search. */
static int contains_ci(const char *haystack, size_t hay_len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return 1;
    }
    if (nlen > hay_len) {
        return 0;
    }
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)haystack[i + j]) ==
                   tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

char *plan_list_sections(const plan_doc *doc)
{
    /* Worst case output is bounded by the input size. */
    char *out = malloc(doc->len + 2U);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    const char *p = doc->text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        if (heading_level(p) > 0) {
            memcpy(out + w, p, line_len);
            w += line_len;
            out[w++] = '\n';
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    out[w] = '\0';
    return out;
}

/* Does the heading title match key? A leading token match ("5.3", "5") is
 * tried first, then a case-insensitive substring of the whole title. */
static int title_matches(const char *title, size_t title_len, const char *key)
{
    size_t klen = strlen(key);
    /* Leading token: key followed by a space, '.', or the section separator. */
    if (klen <= title_len && strncmp(title, key, klen) == 0) {
        char after = title[klen];
        if (after == ' ' || after == '.' || after == '\0' || after == ':') {
            return 1;
        }
    }
    return contains_ci(title, title_len, key);
}

char *plan_get_section(const plan_doc *doc, const char *key)
{
    const char *p = doc->text;
    const char *sec_start = NULL;
    int sec_level = 0;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        int level = heading_level(p);

        if (sec_start != NULL) {
            /* End the section at the next heading of same-or-higher level. */
            if (level > 0 && level <= sec_level) {
                size_t n = (size_t)(p - sec_start);
                char *out = malloc(n + 1U);
                if (out == NULL) {
                    return NULL;
                }
                memcpy(out, sec_start, n);
                out[n] = '\0';
                return out;
            }
        } else if (level > 0) {
            const char *title = heading_title(p, level);
            size_t title_len = line_len - (size_t)(title - p);
            if (title_matches(title, title_len, key)) {
                sec_start = p;
                sec_level = level;
            }
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (sec_start != NULL) {
        /* Section runs to end of file. */
        size_t n = strlen(sec_start);
        char *out = malloc(n + 1U);
        if (out == NULL) {
            return NULL;
        }
        memcpy(out, sec_start, n);
        out[n] = '\0';
        return out;
    }

    return strdup("No section matching that key was found. "
                  "Use list_sections to see available headings.");
}

char *plan_get_phase(const plan_doc *doc, int phase)
{
    char needle[32];
    (void)snprintf(needle, sizeof needle, "Phase %d ", phase);

    char *sec = plan_get_section(doc, needle);
    if (sec == NULL) {
        return NULL;
    }
    /* If the substring match missed (e.g. "Phase 5 —" spacing), fall back to a
     * looser "Phase %d" probe. */
    if (strncmp(sec, "No section", 10U) == 0) {
        free(sec);
        (void)snprintf(needle, sizeof needle, "Phase %d", phase);
        sec = plan_get_section(doc, needle);
    }
    return sec;
}

char *plan_list_decisions(const plan_doc *doc)
{
    /* The plan enumerates decisions 0000-0003; report them with the status it
     * records. This is a fixed, curated view rather than a scrape, so it stays
     * stable even as prose around each decision changes. */
    (void)doc;
    return strdup(
        "Design-journal decisions referenced by the plan (see \u00a715, \u00a717):\n"
        "  0000 - Project name \"Joist\": settled (accepted).\n"
        "  0001 - Native IPC model (sync + notifications, badge and lifetime\n"
        "         semantics): required next; blocks Phase 3.\n"
        "  0002 - Clang-vs-GCC for glibc, sized against a one-month budget\n"
        "         (\u00a713): to be recorded from the Phase 0 measurement.\n"
        "  0003 - SMP + no-runtime-allocation + bounded WCET taken together\n"
        "         (\u00a75.6): must be settled before Phase 3 IPC work begins.\n"
        "New decisions go in docs/decisions/NNNN-title.md using the \u00a715 template.");
}
