#pragma once

/*
 * Recognition hints ("辨識提示"): the optional session.start.context the
 * server uses to bias recognition -- a dictionary profile kept on the server,
 * a short description of the setting (domain), hotwords, and a replacement
 * table for known mis-hearings.
 *
 * This header holds the plain parts, testable without OBS or Qt:
 *
 *   - the text formats the Properties window and the hints file use
 *     (tea_hints_parse_hotwords(), tea_hints_parse_replacements(),
 *     tea_hints_parse_file());
 *   - the merge order: the file first, then the text fields (hotwords are
 *     unioned, a field's replacement overrides the file's for the same
 *     `from`);
 *   - truncation to the limits the server advertises
 *     (capabilities.features.context_limits, tea_hints_apply_limits()).
 *
 * The client (asr-client.cpp) re-reads the file and rebuilds the hints for
 * every session.start, and sends them only when the server advertises
 * features.context_biasing.
 *
 * Plain C11, static inline, also compiled as C++17 (no designated
 * initializers, explicit casts from void *). Memory comes from bmem.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <util/bmem.h>

/* capabilities.features.context_biasing, as last seen */
#define TEA_HINTS_CAP_UNKNOWN 0 /* no capabilities yet */
#define TEA_HINTS_CAP_ABSENT 1  /* the server does not know the feature */
#define TEA_HINTS_CAP_OFF 2     /* known, turned off on the server (TEA_ASR_CONTEXT_HINTS) */
#define TEA_HINTS_CAP_ON 3

/* capabilities.features.context_limits. A value <= 0 was not advertised:
 * that kind is not limited here (the server still enforces its own). */
typedef struct {
	int max_domain_chars;
	int max_hotwords;
	int max_hotword_chars;
	int max_replacements;
	int max_replacement_chars;
} tea_hints_limits_t;

typedef struct {
	char *from;
	char *to; /* "" = delete what `from` matches */
} tea_hint_pair_t;

#define TEA_HINTS_ISSUES_MAX 512

typedef struct {
	char **hotwords;
	int hotword_count;
	int hotword_cap;
	tea_hint_pair_t *pairs;
	int pair_count;
	int pair_cap;
	/* replacement lines that are not "from => to" (or have an empty left
	 * side); the first few are listed, newline separated */
	int invalid_lines;
	char invalid[TEA_HINTS_ISSUES_MAX];
} tea_hints_t;

/* What tea_hints_apply_limits() had to cut. */
typedef struct {
	int domain_chars; /* before truncation */
	int domain_cut;   /* characters cut off the end of the domain */
	int hotwords_over;
	int replacements_over;
	int too_long; /* entries dropped for being longer than their per-entry limit */
	char too_long_list[TEA_HINTS_ISSUES_MAX];
} tea_hints_report_t;

static inline void tea_hints_init(tea_hints_t *h)
{
	memset(h, 0, sizeof(*h));
}

static inline void tea_hints_free(tea_hints_t *h)
{
	for (int i = 0; i < h->hotword_count; i++)
		bfree(h->hotwords[i]);
	bfree(h->hotwords);
	for (int i = 0; i < h->pair_count; i++) {
		bfree(h->pairs[i].from);
		bfree(h->pairs[i].to);
	}
	bfree(h->pairs);
	tea_hints_init(h);
}

/* Code points in s[0, len) (what the server's limits count). */
static inline int tea_hints_chars(const char *s, size_t len)
{
	int n = 0;
	for (size_t i = 0; i < len; i++)
		if (((unsigned char)s[i] & 0xC0u) != 0x80u)
			n++;
	return n;
}

/* Byte length of the first `chars` code points of s[0, len). */
static inline size_t tea_hints_prefix_bytes(const char *s, size_t len, int chars)
{
	int n = 0;
	for (size_t i = 0; i < len; i++) {
		if (((unsigned char)s[i] & 0xC0u) != 0x80u) {
			if (n == chars)
				return i;
			n++;
		}
	}
	return len;
}

static inline char *tea_hints_dup(const char *s, size_t len)
{
	char *out = (char *)bmalloc(len + 1);
	memcpy(out, s, len);
	out[len] = '\0';
	return out;
}

/* Bytes of whitespace at s: ASCII, U+3000 (ideographic space), a UTF-8 BOM. */
static inline size_t tea_hints_space_at(const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *)s;
	if (len >= 1 && (p[0] == ' ' || p[0] == '\t' || p[0] == '\r' || p[0] == '\n' || p[0] == '\v' || p[0] == '\f'))
		return 1;
	if (len >= 3 && p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80)
		return 3;
	if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
		return 3;
	return 0;
}

static inline void tea_hints_trim(const char **s, size_t *len)
{
	size_t k;
	while (*len > 0 && (k = tea_hints_space_at(*s, *len)) > 0) {
		*s += k;
		*len -= k;
	}
	for (;;) {
		if (*len >= 1 && tea_hints_space_at(*s + *len - 1, 1) == 1)
			*len -= 1;
		else if (*len >= 3 && tea_hints_space_at(*s + *len - 3, 3) == 3)
			*len -= 3;
		else
			break;
	}
}

/* Next line of *p (LF, CRLF or CR endings); false at the end. */
static inline bool tea_hints_next_line(const char **p, const char **line, size_t *len)
{
	const char *s = *p;
	if (!s || !*s)
		return false;
	const char *e = s;
	while (*e && *e != '\n' && *e != '\r')
		e++;
	*line = s;
	*len = (size_t)(e - s);
	if (*e == '\r' && e[1] == '\n')
		e += 2;
	else if (*e)
		e++;
	*p = e;
	return true;
}

static inline void tea_hints_note(char *list, const char *s, size_t len)
{
	size_t used = strlen(list);
	if (used + len + 2 >= TEA_HINTS_ISSUES_MAX)
		return;
	if (used > 0)
		list[used++] = '\n';
	memcpy(list + used, s, len);
	list[used + len] = '\0';
}

static inline void tea_hints_add_hotword(tea_hints_t *h, const char *s, size_t len)
{
	for (int i = 0; i < h->hotword_count; i++)
		if (strlen(h->hotwords[i]) == len && memcmp(h->hotwords[i], s, len) == 0)
			return; /* union: each word once */
	if (h->hotword_count == h->hotword_cap) {
		int cap = h->hotword_cap ? h->hotword_cap * 2 : 16;
		char **grown = (char **)bmalloc(sizeof(char *) * (size_t)cap);
		if (h->hotword_count)
			memcpy(grown, h->hotwords, sizeof(char *) * (size_t)h->hotword_count);
		bfree(h->hotwords);
		h->hotwords = grown;
		h->hotword_cap = cap;
	}
	h->hotwords[h->hotword_count++] = tea_hints_dup(s, len);
}

/* A later pair for the same `from` replaces the earlier one in place. */
static inline void tea_hints_set_pair(tea_hints_t *h, const char *from, size_t from_len, const char *to, size_t to_len)
{
	for (int i = 0; i < h->pair_count; i++) {
		if (strlen(h->pairs[i].from) == from_len && memcmp(h->pairs[i].from, from, from_len) == 0) {
			bfree(h->pairs[i].to);
			h->pairs[i].to = tea_hints_dup(to, to_len);
			return;
		}
	}
	if (h->pair_count == h->pair_cap) {
		int cap = h->pair_cap ? h->pair_cap * 2 : 16;
		tea_hint_pair_t *grown = (tea_hint_pair_t *)bmalloc(sizeof(tea_hint_pair_t) * (size_t)cap);
		if (h->pair_count)
			memcpy(grown, h->pairs, sizeof(tea_hint_pair_t) * (size_t)h->pair_count);
		bfree(h->pairs);
		h->pairs = grown;
		h->pair_cap = cap;
	}
	h->pairs[h->pair_count].from = tea_hints_dup(from, from_len);
	h->pairs[h->pair_count].to = tea_hints_dup(to, to_len);
	h->pair_count++;
}

/* A trimmed line that is empty or starts with '#' carries nothing. */
static inline bool tea_hints_skip_line(const char *s, size_t len)
{
	return len == 0 || s[0] == '#';
}

/*
 * One replacement line (already trimmed, not a comment):
 *   錯字 => 正字     錯字=>正字     錯字<TAB>正字
 * A '#' after whitespace starts a trailing comment ("C 井 => C#" keeps its
 * '#'). An empty right side means "delete". Returns false (and records the
 * line) when it is not a pair.
 */
static inline bool tea_hints_replacement_line(tea_hints_t *h, const char *s, size_t len)
{
	for (size_t i = 1; i < len; i++) {
		if (s[i] == '#' && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
			len = i;
			tea_hints_trim(&s, &len);
			break;
		}
	}
	size_t sep = len, sep_len = 0;
	for (size_t i = 0; i + 1 < len; i++) {
		if (s[i] == '=' && s[i + 1] == '>') {
			sep = i;
			sep_len = 2;
			break;
		}
	}
	if (!sep_len) {
		for (size_t i = 0; i < len; i++) {
			if (s[i] == '\t') {
				sep = i;
				sep_len = 1;
				break;
			}
		}
	}
	const char *from = s, *to = s + sep + sep_len;
	size_t from_len = sep, to_len = sep_len ? len - sep - sep_len : 0;
	tea_hints_trim(&from, &from_len);
	tea_hints_trim(&to, &to_len);
	if (!sep_len || from_len == 0) {
		h->invalid_lines++;
		tea_hints_note(h->invalid, s, len);
		return false;
	}
	tea_hints_set_pair(h, from, from_len, to, to_len);
	return true;
}

/* 專有詞 field: one hotword per line; blank lines and '#' lines are skipped. */
static inline void tea_hints_parse_hotwords(tea_hints_t *h, const char *text)
{
	const char *p = text, *line;
	size_t len;
	while (tea_hints_next_line(&p, &line, &len)) {
		tea_hints_trim(&line, &len);
		if (!tea_hints_skip_line(line, len))
			tea_hints_add_hotword(h, line, len);
	}
}

/* 對照表 field: one "錯字 => 正字" pair per line. */
static inline void tea_hints_parse_replacements(tea_hints_t *h, const char *text)
{
	const char *p = text, *line;
	size_t len;
	while (tea_hints_next_line(&p, &line, &len)) {
		tea_hints_trim(&line, &len);
		if (!tea_hints_skip_line(line, len))
			tea_hints_replacement_line(h, line, len);
	}
}

#define TEA_HINTS_SECTION_NONE 0
#define TEA_HINTS_SECTION_HOTWORDS 1
#define TEA_HINTS_SECTION_REPLACEMENTS 2
#define TEA_HINTS_SECTION_OTHER 3

static inline bool tea_hints_ascii_ieq(const char *s, size_t len, const char *word)
{
	if (strlen(word) != len)
		return false;
	for (size_t i = 0; i < len; i++) {
		char a = s[i];
		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != word[i])
			return false;
	}
	return true;
}

static inline bool tea_hints_bytes_eq(const char *s, size_t len, const char *word)
{
	return strlen(word) == len && memcmp(s, word, len) == 0;
}

/* "[專有詞]" / "[hotwords]", "[對照表]" / "[replacements]", other "[...]". */
static inline int tea_hints_section(const char *s, size_t len)
{
	if (len < 2 || s[0] != '[' || s[len - 1] != ']')
		return TEA_HINTS_SECTION_NONE;
	const char *name = s + 1;
	size_t name_len = len - 2;
	tea_hints_trim(&name, &name_len);
	if (tea_hints_bytes_eq(name, name_len, "專有詞") || tea_hints_ascii_ieq(name, name_len, "hotwords"))
		return TEA_HINTS_SECTION_HOTWORDS;
	if (tea_hints_bytes_eq(name, name_len, "對照表") || tea_hints_ascii_ieq(name, name_len, "replacements"))
		return TEA_HINTS_SECTION_REPLACEMENTS;
	return TEA_HINTS_SECTION_OTHER;
}

/*
 * A hints file (UTF-8, BOM allowed):
 *
 *   # comments anywhere
 *   [專有詞]
 *   聖經
 *   以弗所書
 *   [對照表]
 *   盛家 => 聖經
 *
 * Lines outside a known section are ignored. Returns the number of lines
 * that were used.
 */
static inline int tea_hints_parse_file(tea_hints_t *h, const char *text)
{
	const char *p = text, *line;
	size_t len;
	int section = TEA_HINTS_SECTION_NONE, used = 0;
	while (tea_hints_next_line(&p, &line, &len)) {
		tea_hints_trim(&line, &len);
		if (tea_hints_skip_line(line, len))
			continue;
		const int next = tea_hints_section(line, len);
		if (next != TEA_HINTS_SECTION_NONE) {
			section = next;
			continue;
		}
		if (section == TEA_HINTS_SECTION_HOTWORDS) {
			tea_hints_add_hotword(h, line, len);
			used++;
		} else if (section == TEA_HINTS_SECTION_REPLACEMENTS) {
			if (tea_hints_replacement_line(h, line, len))
				used++;
		}
	}
	return used;
}

/*
 * The hints to send: the file's (NULL = none) first, then the text fields,
 * which add hotwords and override the file's replacements by `from`.
 */
static inline void tea_hints_build(tea_hints_t *h, const char *file_text, const char *hotwords_field,
				   const char *replacements_field)
{
	tea_hints_init(h);
	if (file_text)
		tea_hints_parse_file(h, file_text);
	if (hotwords_field)
		tea_hints_parse_hotwords(h, hotwords_field);
	if (replacements_field)
		tea_hints_parse_replacements(h, replacements_field);
}

/* The domain text as sent: trimmed; *len gets its byte length. */
static inline const char *tea_hints_domain(const char *domain, size_t *len)
{
	const char *s = domain ? domain : "";
	*len = strlen(s);
	tea_hints_trim(&s, len);
	return s;
}

static inline void tea_hints_note_too_long(tea_hints_report_t *r, const char *s)
{
	r->too_long++;
	tea_hints_note(r->too_long_list, s, strlen(s));
}

/*
 * Cuts `h` down to `limits` and fills `report`; *domain_keep gets the byte
 * length of the domain (from tea_hints_domain()) that fits. Entries longer
 * than their per-entry limit are dropped whole and listed (a cut word would
 * be a different word); beyond the count limits the first ones are kept, so
 * file entries win over field entries.
 */
static inline void tea_hints_apply_limits(tea_hints_t *h, const char *domain, size_t domain_len,
					  const tea_hints_limits_t *limits, size_t *domain_keep,
					  tea_hints_report_t *report)
{
	memset(report, 0, sizeof(*report));
	report->domain_chars = tea_hints_chars(domain, domain_len);
	*domain_keep = domain_len;
	if (limits->max_domain_chars > 0 && report->domain_chars > limits->max_domain_chars) {
		*domain_keep = tea_hints_prefix_bytes(domain, domain_len, limits->max_domain_chars);
		report->domain_cut = report->domain_chars - limits->max_domain_chars;
	}

	int kept = 0;
	for (int i = 0; i < h->hotword_count; i++) {
		char *w = h->hotwords[i];
		const bool too_long = limits->max_hotword_chars > 0 &&
				      tea_hints_chars(w, strlen(w)) > limits->max_hotword_chars;
		if (too_long) {
			tea_hints_note_too_long(report, w);
			bfree(w);
		} else if (limits->max_hotwords > 0 && kept >= limits->max_hotwords) {
			report->hotwords_over++;
			bfree(w);
		} else {
			h->hotwords[kept++] = w;
		}
	}
	h->hotword_count = kept;

	kept = 0;
	for (int i = 0; i < h->pair_count; i++) {
		tea_hint_pair_t pair = h->pairs[i];
		const int max = limits->max_replacement_chars;
		const bool too_long = max > 0 && (tea_hints_chars(pair.from, strlen(pair.from)) > max ||
						  tea_hints_chars(pair.to, strlen(pair.to)) > max);
		if (!too_long && (limits->max_replacements <= 0 || kept < limits->max_replacements)) {
			h->pairs[kept++] = pair;
			continue;
		}
		if (too_long)
			tea_hints_note_too_long(report, pair.from);
		else
			report->replacements_over++;
		bfree(pair.from);
		bfree(pair.to);
	}
	h->pair_count = kept;
}

static inline bool tea_hints_report_cut(const tea_hints_report_t *r)
{
	return r->domain_cut > 0 || r->hotwords_over > 0 || r->replacements_over > 0 || r->too_long > 0;
}

/* "（不使用）" / "(none)" (either language) or blank: no profile. */
static inline bool tea_hints_profile_is_none(const char *profile)
{
	const char *s = profile ? profile : "";
	size_t len = strlen(s);
	tea_hints_trim(&s, &len);
	return len == 0 || tea_hints_bytes_eq(s, len, "（不使用）") || tea_hints_bytes_eq(s, len, "(不使用)") ||
	       tea_hints_ascii_ieq(s, len, "(none)");
}

static inline bool tea_hints_contains_ci(const char *s, const char *word)
{
	const size_t n = strlen(word);
	for (const char *p = s ? s : ""; *p; p++)
		if (tea_hints_ascii_ieq(p, strlen(p) < n ? strlen(p) : n, word))
			return true;
	return false;
}

/*
 * Whether a server `error` answering a session.start that carried a context
 * is about that context. The context is the newest session.start field, so
 * an unsupported_option / protocol_error is blamed on it unless the message
 * names another field; any error that names the context, a profile, a
 * dictionary, hotwords or replacements is about it whatever its code
 * (an unknown profile, a limit exceeded).
 */
static inline bool tea_hints_error_is_context(const char *code, const char *message)
{
	static const char *const about[] = {"context", "profile", "dictionar", "hotword", "replacement", "domain"};
	for (size_t i = 0; i < sizeof(about) / sizeof(about[0]); i++)
		if (tea_hints_contains_ci(code, about[i]) || tea_hints_contains_ci(message, about[i]))
			return true;
	const bool generic = code && (strcmp(code, "unsupported_option") == 0 || strcmp(code, "protocol_error") == 0);
	return generic && !tea_hints_contains_ci(message, "segmentation") && !tea_hints_contains_ci(message, "stable");
}

/* Whether any of the old inline hint settings (the setting description,
 * hotwords, replacement table, hints file) holds something. The Properties
 * window no longer shows them -- dictionaries are edited centrally in the
 * TEA ASR app -- but a source that has them still sends them. */
static inline bool tea_hints_has_legacy(const char *domain, const char *hotwords, const char *replacements,
					const char *file_path)
{
	const char *fields[] = {domain, hotwords, replacements, file_path};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		const char *s = fields[i] ? fields[i] : "";
		size_t len = strlen(s);
		tea_hints_trim(&s, &len);
		if (len > 0)
			return true;
	}
	return false;
}
