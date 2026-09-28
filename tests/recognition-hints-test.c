/* Tests for src/recognition-hints.h: the 辨識提示 text formats, the hints
 * file, the merge order (file first, then the fields) and the limits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recognition-hints.h"

static int failures = 0;

static void expect(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static bool has_hotword(const tea_hints_t *h, const char *word)
{
	for (int i = 0; i < h->hotword_count; i++)
		if (strcmp(h->hotwords[i], word) == 0)
			return true;
	return false;
}

/* The `to` of `from`, or NULL. */
static const char *pair_to(const tea_hints_t *h, const char *from)
{
	for (int i = 0; i < h->pair_count; i++)
		if (strcmp(h->pairs[i].from, from) == 0)
			return h->pairs[i].to;
	return NULL;
}

static void test_hotwords(void)
{
	tea_hints_t h;
	tea_hints_init(&h);
	tea_hints_parse_hotwords(&h, "聖經\r\n\r\n  以弗所書  \n# 這是註解\n\t\n聖經\r哥林多前書\n　聖靈　\n");
	expect(h.hotword_count == 4, "blank lines, comments and duplicates are skipped (CRLF, CR, LF)");
	expect(has_hotword(&h, "聖經") && has_hotword(&h, "以弗所書") && has_hotword(&h, "哥林多前書"),
	       "each line is one hotword, trimmed");
	expect(has_hotword(&h, "聖靈"), "an ideographic space is trimmed too");
	expect(strcmp(h.hotwords[0], "聖經") == 0 && strcmp(h.hotwords[1], "以弗所書") == 0, "order is kept");
	tea_hints_free(&h);

	tea_hints_init(&h);
	tea_hints_parse_hotwords(&h, "");
	tea_hints_parse_hotwords(&h, NULL);
	expect(h.hotword_count == 0, "empty or missing text gives no hotwords");
	tea_hints_free(&h);
}

static void test_replacements(void)
{
	tea_hints_t h;
	tea_hints_init(&h);
	tea_hints_parse_replacements(&h, "盛家 => 聖經\r\n"
					 "聖家=>聖經\n"
					 "以弗所書\t以弗所書\n"
					 "# 註解 => 不算\n"
					 "\n"
					 "呃 =>\n"
					 "C 井 => C#\n"
					 "阿們 => 阿們 # 保留\n"
					 "沒有分隔\n"
					 " => 左邊空的\n"
					 "盛家 => 聖經書\n");
	expect(pair_to(&h, "盛家") && strcmp(pair_to(&h, "盛家"), "聖經書") == 0,
	       "' => ' form; a later line for the same `from` overrides");
	expect(pair_to(&h, "聖家") && strcmp(pair_to(&h, "聖家"), "聖經") == 0, "'=>' without spaces");
	expect(pair_to(&h, "以弗所書") && strcmp(pair_to(&h, "以弗所書"), "以弗所書") == 0, "tab-separated form");
	expect(pair_to(&h, "呃") && strcmp(pair_to(&h, "呃"), "") == 0, "an empty right side means delete");
	expect(pair_to(&h, "C 井") && strcmp(pair_to(&h, "C 井"), "C#") == 0, "a '#' inside a word is kept");
	expect(pair_to(&h, "阿們") && strcmp(pair_to(&h, "阿們"), "阿們") == 0, "' #' starts a trailing comment");
	expect(pair_to(&h, "# 註解") == NULL, "a '#' line is a comment");
	expect(h.pair_count == 6, "six pairs");
	expect(h.invalid_lines == 2, "a line without a separator and one with an empty left side are invalid");
	expect(strstr(h.invalid, "沒有分隔") != NULL && strstr(h.invalid, "左邊空的") != NULL,
	       "invalid lines are listed");
	tea_hints_free(&h);
}

static const char k_file[] = "\xEF\xBB\xBF# 我的講道提示檔\r\n"
			     "這一行在任何段落之前，不算\r\n"
			     "[專有詞]\r\n"
			     "聖經\r\n"
			     "以弗所書\r\n"
			     "\r\n"
			     "[對照表]\r\n"
			     "盛家 => 聖經\r\n"
			     "星際 => 聖經\r\n"
			     "[其他]\r\n"
			     "不認得的段落 => 不算\r\n"
			     "[ hotwords ]\r\n"
			     "Amen\r\n"
			     "[Replacements]\r\n"
			     "阿門 => 阿們\r\n";

static void test_file(void)
{
	tea_hints_t h;
	tea_hints_init(&h);
	const int used = tea_hints_parse_file(&h, k_file);
	expect(used == 6, "six lines used from the file");
	expect(h.hotword_count == 3 && has_hotword(&h, "聖經") && has_hotword(&h, "以弗所書") &&
		       has_hotword(&h, "Amen"),
	       "[專有詞] and [hotwords] sections");
	expect(h.pair_count == 3 && pair_to(&h, "盛家") && pair_to(&h, "阿門") && pair_to(&h, "星際"),
	       "[對照表] and [replacements] sections");
	expect(!has_hotword(&h, "這一行在任何段落之前，不算") && pair_to(&h, "不認得的段落") == NULL,
	       "lines before a section or in an unknown section are ignored");
	tea_hints_free(&h);
}

static void test_merge_order(void)
{
	tea_hints_t h;
	tea_hints_build(&h, k_file, "聖靈\n聖經\n", "盛家 => 聖經課\n恩點 => 恩典\n");
	expect(h.hotword_count == 4, "hotwords are the union of the file and the field");
	expect(strcmp(h.hotwords[0], "聖經") == 0 && strcmp(h.hotwords[3], "聖靈") == 0,
	       "the file's hotwords come first");
	expect(strcmp(pair_to(&h, "盛家"), "聖經課") == 0,
	       "a field replacement overrides the file's for the same from");
	expect(strcmp(pair_to(&h, "星際"), "聖經") == 0 && strcmp(pair_to(&h, "恩點"), "恩典") == 0,
	       "other pairs from both are kept");
	tea_hints_free(&h);

	tea_hints_build(&h, NULL, NULL, NULL);
	expect(h.hotword_count == 0 && h.pair_count == 0, "nothing configured gives nothing");
	tea_hints_free(&h);
}

static void test_limits(void)
{
	tea_hints_limits_t limits;
	limits.max_domain_chars = 5;
	limits.max_hotwords = 2;
	limits.max_hotword_chars = 4;
	limits.max_replacements = 2;
	limits.max_replacement_chars = 3;

	tea_hints_t h;
	tea_hints_build(&h, NULL, "聖經\n以弗所書\n太長的一個專有詞\n聖靈\n恩典\n",
			"盛家 => 聖經\n太長的錯字 => 對\n錯 => 太長的正字\n呃 =>\n恩點 => 恩典\n");
	size_t domain_len = 0;
	const char *domain = tea_hints_domain("  主日講道：以弗所書  ", &domain_len);
	expect(tea_hints_chars(domain, domain_len) == 9, "the domain is trimmed and counted in code points");
	size_t keep = 0;
	tea_hints_report_t report;
	tea_hints_apply_limits(&h, domain, domain_len, &limits, &keep, &report);

	expect(report.domain_chars == 9 && report.domain_cut == 4, "the domain is cut to its limit");
	expect(keep == strlen("主日講道：") && memcmp(domain, "主日講道：", keep) == 0,
	       "the domain is cut on a character boundary");
	expect(h.hotword_count == 2 && strcmp(h.hotwords[0], "聖經") == 0 && strcmp(h.hotwords[1], "以弗所書") == 0,
	       "hotwords: the first ones up to the count limit");
	expect(report.hotwords_over == 2, "two hotwords over the count limit");
	expect(h.pair_count == 2 && pair_to(&h, "盛家") && pair_to(&h, "呃"), "replacements up to the count limit");
	expect(report.replacements_over == 1, "one replacement over the count limit");
	expect(report.too_long == 3, "entries longer than their limit are dropped whole");
	expect(strstr(report.too_long_list, "太長的一個專有詞") && strstr(report.too_long_list, "太長的錯字") &&
		       strstr(report.too_long_list, "錯"),
	       "and listed");
	expect(tea_hints_report_cut(&report), "the report says something was cut");
	tea_hints_free(&h);

	/* within the limits nothing changes */
	tea_hints_build(&h, NULL, "聖經\n", "盛家 => 聖經\n");
	domain = tea_hints_domain("講道", &domain_len);
	tea_hints_apply_limits(&h, domain, domain_len, &limits, &keep, &report);
	expect(!tea_hints_report_cut(&report) && keep == domain_len && h.hotword_count == 1 && h.pair_count == 1,
	       "nothing is cut within the limits");
	tea_hints_free(&h);

	/* limits the server did not advertise do not cut */
	tea_hints_limits_t none;
	memset(&none, 0, sizeof(none));
	tea_hints_build(&h, NULL, "太長的一個專有詞\n", NULL);
	domain = tea_hints_domain("很長很長的一段說明", &domain_len);
	tea_hints_apply_limits(&h, domain, domain_len, &none, &keep, &report);
	expect(!tea_hints_report_cut(&report) && h.hotword_count == 1 && keep == domain_len,
	       "no advertised limit, no cut");
	tea_hints_free(&h);
}

static void test_profile(void)
{
	expect(tea_hints_profile_is_none(NULL) && tea_hints_profile_is_none("") && tea_hints_profile_is_none("  "),
	       "blank is no profile");
	expect(tea_hints_profile_is_none("（不使用）") && tea_hints_profile_is_none("(none)") &&
		       tea_hints_profile_is_none("(None)"),
	       "the combobox's none entry in either language is no profile");
	expect(!tea_hints_profile_is_none("church"), "a name is a profile");
}

static void test_error_attribution(void)
{
	expect(tea_hints_error_is_context("unsupported_option", "Extra inputs are not permitted"),
	       "a generic rejection is blamed on the newest field, the context");
	expect(tea_hints_error_is_context("protocol_error", "context.hotwords: too many"), "a message naming it");
	expect(tea_hints_error_is_context("unknown_profile", "no dictionary named church"), "an unknown profile");
	expect(tea_hints_error_is_context("invalid_request", "Profile 'x' not found"), "any code naming the profile");
	expect(!tea_hints_error_is_context("unsupported_option", "segmentation.end_silence_ms out of range"),
	       "a rejection naming segmentation is not about the context");
	expect(!tea_hints_error_is_context("protocol_error", "stable requires transcript_mode=revisable"),
	       "nor one naming stable");
	expect(!tea_hints_error_is_context("concurrent_session_limit", "too many sessions"), "nor admission errors");
	expect(!tea_hints_error_is_context(NULL, NULL), "nothing is nothing");
}

int main(void)
{
	test_error_attribution();
	test_hotwords();
	test_replacements();
	test_file();
	test_merge_order();
	test_limits();
	test_profile();
	if (failures) {
		fprintf(stderr, "%d recognition hints test(s) failed\n", failures);
		return 1;
	}
	printf("recognition hints tests passed\n");
	return 0;
}
