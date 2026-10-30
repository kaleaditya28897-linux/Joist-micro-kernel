/* SPDX-License-Identifier: GPL-3.0-only */
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
    doc->dir = NULL;

    /* The directory part of path ("." if none): where docs/ lives. */
    const char *slash = strrchr(path, '/');
    size_t dlen = slash == NULL ? 1U : (size_t)(slash - path);
    doc->dir = malloc(dlen + 1U);
    if (doc->dir == NULL) {
        return -1;
    }
    if (slash == NULL) {
        doc->dir[0] = '.';
    } else {
        memcpy(doc->dir, path, dlen);
    }
    doc->dir[dlen] = '\0';

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
    free(doc->dir);
    doc->dir = NULL;
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

/* Append src to the growing buffer *buf (length *len, capacity *cap). */
static int append(char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    if (*len + n + 1U > *cap) {
        size_t ncap = (*cap == 0U ? 1024U : *cap * 2U);
        while (ncap < *len + n + 1U) {
            ncap *= 2U;
        }
        char *nb = realloc(*buf, ncap);
        if (nb == NULL) {
            return -1;
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

char *plan_list_decisions(const plan_doc *doc)
{
    char path[4096];
    const char *dir = doc->dir != NULL ? doc->dir : ".";
    (void)snprintf(path, sizeof path, "%s/docs/decisions/README.md", dir);
    plan_doc idx;
    if (plan_load(&idx, path) != 0) {
        char msg[4200];
        (void)snprintf(msg, sizeof msg,
                       "The decision index could not be read at %s.\n"
                       "New decisions go in docs/decisions/NNNN-title.md using the \u00a715 template.",
                       path);
        return strdup(msg);
    }
    /* The index table: rows "| NNNN | Title | Status | Blocks |" after the
     * "## Index" heading, up to the next "## ". */
    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;
    const char *head = "Design-journal decisions (from docs/decisions/README.md):\n";
    if (append(&out, &len, &cap, head, strlen(head)) != 0) {
        plan_free(&idx);
        return NULL;
    }
    const char *p = strstr(idx.text, "\n## Index");
    int rows = 0;
    while (p != NULL && *p != '\0') {
        const char *eol = strchr(p + 1, '\n');
        const char *line = p + 1;
        size_t n = eol != NULL ? (size_t)(eol - line) : strlen(line);
        if (rows > 0 && n >= 3U && strncmp(line, "## ", 3U) == 0) {
            break;
        }
        if (n > 7U && line[0] == '|' && line[1] == ' ' && isdigit((unsigned char)line[2]) &&
            isdigit((unsigned char)line[5])) {
            /* "| 0001 | Title | Status | Blocks |" -> "  0001 - Title: status (blocks)" */
            char row[1024];
            size_t m = n < sizeof row - 1U ? n : sizeof row - 1U;
            memcpy(row, line, m);
            row[m] = '\0';
            char *cell[4] = {NULL, NULL, NULL, NULL};
            int k = 0;
            for (char *t = strtok(row, "|"); t != NULL && k < 4; t = strtok(NULL, "|")) {
                while (*t == ' ') {
                    t++;
                }
                char *e = t + strlen(t);
                while (e > t && e[-1] == ' ') {
                    *--e = '\0';
                }
                cell[k++] = t;
            }
            if (k >= 3) {
                char item[1200];
                (void)snprintf(item, sizeof item, "  %s - %s: %s%s%s%s\n", cell[0], cell[1],
                               cell[2], (k == 4 && strcmp(cell[3], "\u2014") != 0) ? " (blocks " : "",
                               (k == 4 && strcmp(cell[3], "\u2014") != 0) ? cell[3] : "",
                               (k == 4 && strcmp(cell[3], "\u2014") != 0) ? ")" : "");
                if (append(&out, &len, &cap, item, strlen(item)) != 0) {
                    free(out);
                    plan_free(&idx);
                    return NULL;
                }
                rows++;
            }
        }
        p = eol;
    }
    plan_free(&idx);
    const char *tail = rows > 0 ? "New decisions go in docs/decisions/NNNN-title.md using the \u00a715 template."
                                : "No index rows were found in docs/decisions/README.md.";
    if (append(&out, &len, &cap, tail, strlen(tail)) != 0) {
        free(out);
        return NULL;
    }
    return out;
}
