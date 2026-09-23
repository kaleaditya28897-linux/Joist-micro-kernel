/* plan.h - loading and querying the Joist design plan (joist-microkernel-plan.md). */
#ifndef JOIST_MCP_PLAN_H
#define JOIST_MCP_PLAN_H

#include <stddef.h>

/* The whole plan file, loaded once into memory. Owns its buffer. */
typedef struct {
    char  *text;   /* NUL-terminated file contents. */
    size_t len;    /* Length in bytes, excluding the terminator. */
} plan_doc;

/* Load the plan from path. Returns 0 on success, -1 on failure (errno set). */
int  plan_load(plan_doc *doc, const char *path);
void plan_free(plan_doc *doc);

/* Each of the following returns a freshly malloc'd, NUL-terminated string that
 * the caller must free(), or NULL on allocation failure. They never return an
 * empty result for a valid request; "not found" is reported as human-readable
 * text so it can be surfaced straight to the model. */

/* A newline-separated list of every "# / ## / ###" heading, one per line. */
char *plan_list_sections(const plan_doc *doc);

/* The body of the section whose heading matches key. key may be a section
 * number ("5.3"), a leading token ("5"), or a case-insensitive substring of
 * the heading text ("IPC"). Includes the heading line and everything up to the
 * next heading at the same or higher level. */
char *plan_get_section(const plan_doc *doc, const char *key);

/* The roadmap phase whose heading contains "Phase <n>". */
char *plan_get_phase(const plan_doc *doc, int phase);

/* The design-journal decisions mentioned in the plan (0000-0003), with the
 * status the plan records for each. */
char *plan_list_decisions(const plan_doc *doc);

#endif /* JOIST_MCP_PLAN_H */
