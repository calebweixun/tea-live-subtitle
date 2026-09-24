#include "caption-state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>

static void expect(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(EXIT_FAILURE);
	}
}

static void expect_render(tea_caption_state_t *state, const char *expected, const char *message)
{
	char *rendered = tea_caption_state_render(state);
	expect(rendered != NULL && strcmp(rendered, expected) == 0, message);
	free(rendered);
}

/* ---------------- stable mode (transcript.stable) ---------------- */

/* Asserts the new render only appended to the previous one: every line that
 * was shown keeps its exact bytes, only the last line may have grown and new
 * lines may follow. `prev` is updated. */
static void expect_append_only(tea_caption_state_t *state, char **prev, const char *message)
{
	char *now = tea_caption_state_render(state);
	size_t prev_len = strlen(*prev);
	expect(strncmp(now, *prev, prev_len) == 0, message);
	free(*prev);
	*prev = now;
}

static void test_stable_append_only(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	expect(tea_caption_state_stable_mode(st), "stable mode must be selectable");
	char *prev = bstrdup("");

	tea_caption_state_on_partial(st, "s", "seg-a", 1, "我們需");
	expect_render(st, "", "stable mode never renders a transcript.partial");
	expect(!tea_caption_state_last_line_is_partial(st), "no line yet");

	tea_caption_state_on_stable(st, "s", "seg-a", 0, 1, "我們", "open");
	expect_render(st, "我們", "first committed text renders");
	expect(tea_caption_state_last_line_is_partial(st), "an open stable line may still grow");
	expect_append_only(st, &prev, "open stable appends");

	tea_caption_state_on_partial(st, "s", "seg-a", 2, "我們需要改");
	expect_render(st, "我們", "an uncommitted tail never renders");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 2, "我們需要", "open");
	expect_render(st, "我們需要", "a longer committed prefix appends its tail");
	expect_append_only(st, &prev, "second open stable only appends");

	/* stable_revision gaps are fine (coalesced opens); stale/duplicate are ignored */
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 5, "我們需要一個", "open");
	expect_append_only(st, &prev, "coalesced open (revision gap) appends");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 4, "我們需要一", "open");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 5, "我們需要一個", "open");
	expect_render(st, "我們需要一個", "stale or duplicate stable revisions are ignored");
	expect(tea_caption_state_stable_mismatches(st) == 0, "stale revisions are not contract violations");

	tea_caption_state_on_final(st, "s", "seg-a", 7, "我們需要一個計畫");
	expect_render(st, "我們需要一個計畫", "a final that extends the committed text shows at once");
	expect_append_only(st, &prev, "final extension only appends");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 6, "我們需要一個計畫", "final");
	expect_render(st, "我們需要一個計畫", "closing state=final equals the final: no change");
	expect(!tea_caption_state_last_line_is_partial(st), "state=final closes the line");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 7, "我們需要一個計畫再說", "open");
	expect_render(st, "我們需要一個計畫", "nothing changes a closed segment");
	tea_caption_state_on_partial(st, "s", "seg-a", 9, "x");
	expect_render(st, "我們需要一個計畫", "partial after final is still ignored");

	free(prev);
	tea_caption_state_destroy(st);
}

static void test_stable_segment_isolation(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_max_lines(st, 3);
	tea_caption_state_set_stable_mode(st, true);
	char *prev = bstrdup("");

	tea_caption_state_on_stable(st, "s", "seg-1", 0, 1, "第一段", "open");
	expect_append_only(st, &prev, "segment 1 open");
	/* Segment 2 commits text before segment 1's final arrives. */
	tea_caption_state_on_stable(st, "s", "seg-2", 1, 1, "第二段", "open");
	expect_render(st, "第一段\n第二段", "a later segment gets its own line, never overwrites the earlier one");
	expect_append_only(st, &prev, "segment 2 appends a line");
	tea_caption_state_on_stable(st, "s", "seg-2", 1, 2, "第二段開始", "open");
	expect_render(st, "第一段\n第二段開始", "segment 2 grows on its own line");

	tea_caption_state_on_final(st, "s", "seg-1", 3, "第一段結束");
	tea_caption_state_on_stable(st, "s", "seg-1", 0, 2, "第一段結束", "final");
	expect_render(st, "第一段結束\n第二段開始", "segment 1 closes in place; segment 2 is untouched");

	/* Segment 3 never committed anything before segment 4 did, and its final
	 * comes late: it is shown above segment 4, in transcript order. */
	tea_caption_state_on_stable(st, "s", "seg-4", 3, 1, "第四", "open");
	expect_render(st, "第一段結束\n第二段開始\n第四", "three lines visible");
	tea_caption_state_on_final_indexed(st, "s", "seg-3", 2, 1, "第三");
	expect_render(st, "第二段開始\n第三\n第四", "a late segment is placed by segment_index");
	tea_caption_state_on_stable(st, "s", "seg-3", 2, 1, "第三", "final");
	expect_render(st, "第二段開始\n第三\n第四", "its closing stable is a no-op");

	/* A final with no stable at all (utterance too short to agree) shows. */
	tea_caption_state_on_final_indexed(st, "s", "seg-5", 4, 1, "好");
	tea_caption_state_on_stable(st, "s", "seg-5", 4, 1, "好", "final");
	expect_render(st, "第三\n第四\n好", "a final-only segment is still shown");
	expect(tea_caption_state_stable_mismatches(st) == 0, "normal traffic has no mismatches");

	free(prev);
	tea_caption_state_destroy(st);
}

static void test_stable_closing_states(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_max_lines(st, 3);
	tea_caption_state_set_stable_mode(st, true);

	/* diverged: the final rewrote committed text; the screen keeps it and
	 * only gets the final's tail. */
	tea_caption_state_on_stable(st, "s", "div", 0, 1, "今天天氣", "open");
	tea_caption_state_on_final_indexed(st, "s", "div", 0, 4, "今天天汽很好");
	expect_render(st, "今天天氣", "a non-extending final never rewrites committed text");
	expect(tea_caption_state_last_line_is_partial(st), "the line waits for its diverged closing");
	tea_caption_state_on_stable(st, "s", "div", 0, 2, "今天天氣很好", "diverged");
	expect_render(st, "今天天氣很好", "state=diverged appends the final's tail");
	expect(!tea_caption_state_last_line_is_partial(st), "state=diverged closes the line");
	expect(tea_caption_state_stable_mismatches(st) == 0, "a diverged final is not a mismatch");

	/* abandoned: skipped/error after committed text was shown. */
	tea_caption_state_on_stable(st, "s", "ab", 1, 1, "雜訊", "open");
	tea_caption_state_on_segment_dropped(st, "s", "ab");
	expect_render(st, "今天天氣很好\n雜訊", "segment.skipped keeps committed text");
	expect(!tea_caption_state_last_line_is_partial(st), "a dropped segment stops growing");
	tea_caption_state_on_stable(st, "s", "ab", 1, 2, "雜訊", "abandoned");
	expect_render(st, "今天天氣很好\n雜訊", "state=abandoned keeps the text");
	tea_caption_state_on_stable(st, "s", "ab", 1, 3, "雜訊更多", "open");
	expect_render(st, "今天天氣很好\n雜訊", "nothing reopens an abandoned segment");

	/* open, then session.cancelled: frozen, no closing ever comes. */
	tea_caption_state_on_stable(st, "s", "op", 2, 1, "還沒", "open");
	expect(tea_caption_state_last_line_is_partial(st), "state=open leaves the line open");
	tea_caption_state_on_session_cancelled(st, "s");
	expect_render(st, "今天天氣很好\n雜訊\n還沒", "cancel keeps committed text");
	expect(!tea_caption_state_last_line_is_partial(st), "cancel freezes open lines");

	tea_caption_state_destroy(st);
}

static void test_stable_rejects_non_extension(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);

	tea_caption_state_on_stable(st, "s", "x", 0, 1, "你好世界", "open");
	tea_caption_state_on_stable(st, "s", "x", 0, 2, "你好地球", "open");
	expect_render(st, "你好世界", "a value that does not start with the shown text changes nothing");
	expect(tea_caption_state_stable_mismatches(st) == 1, "the violation is counted");
	tea_caption_state_on_stable(st, "s", "x", 0, 3, "你好", "open");
	expect_render(st, "你好世界", "a shorter value never deletes characters");
	expect(tea_caption_state_stable_mismatches(st) == 2, "shrinking is counted too");
	/* A byte-prefix that breaks a UTF-8 sequence differs in bytes: rejected. */
	tea_caption_state_on_stable(st, "s", "x", 0, 4, "你好世\xe7", "open");
	expect_render(st, "你好世界", "a truncated multi-byte value is rejected");
	tea_caption_state_on_stable(st, "s", "x", 0, 5, "你好世界和平", "diverged");
	expect_render(st, "你好世界和平", "a later valid extension still appends");
	expect(tea_caption_state_stable_mismatches(st) == 3, "count stays for diagnostics");

	tea_caption_state_destroy(st);
}

static void test_stable_utf8_tails(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	char *prev = bstrdup("");

	/* 3-byte CJK, then U+2615 U+FE0F, a skin-tone modifier sequence and a
	 * ZWJ family (4-byte code points joined by U+200D). */
	const char *steps[] = {
		"咖啡",
		"咖啡\xe2\x98\x95\xef\xb8\x8f",
		"咖啡\xe2\x98\x95\xef\xb8\x8f\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd",
		"咖啡\xe2\x98\x95\xef\xb8\x8f\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd"
		"\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7",
		"咖啡\xe2\x98\x95\xef\xb8\x8f\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd"
		"\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"
		"caf\xc3\xa9",
	};
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		tea_caption_state_on_stable(st, "s", "u", 0, (uint64_t)i + 1, steps[i], "open");
		expect_render(st, steps[i], "multi-byte tail appended byte-exactly");
		expect_append_only(st, &prev, "multi-byte tail only appends");
	}
	expect(tea_caption_state_stable_mismatches(st) == 0, "valid multi-byte extensions are not mismatches");

	free(prev);
	tea_caption_state_destroy(st);
}

static void test_stable_hold_for_reconnect(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_max_lines(st, 2);
	tea_caption_state_set_stable_mode(st, true);

	tea_caption_state_on_stable(st, "old", "o1", 0, 1, "斷線前", "open");
	tea_caption_state_hold_for_reconnect(st);
	expect_render(st, "斷線前", "a dropped connection keeps the committed text on screen");
	expect(!tea_caption_state_last_line_is_partial(st), "held lines are frozen");

	/* Same segment_index 0 in the new session sorts below the held line. */
	tea_caption_state_on_stable(st, "new", "n1", 0, 1, "重連後", "open");
	expect_render(st, "斷線前\n重連後", "the new session scrolls in under the held text");
	tea_caption_state_on_stable(st, "new", "n2", 1, 1, "第二句", "open");
	expect_render(st, "重連後\n第二句", "held text scrolls away like any other line");

	/* Without a hold, a different session id still means a fresh canvas. */
	tea_caption_state_on_stable(st, "other", "z", 0, 1, "新", "open");
	expect_render(st, "新", "an unexpected session change still resets");

	tea_caption_state_hold_for_reconnect(st);
	tea_caption_state_reset(st);
	expect_render(st, "", "reset after a long outage clears the canvas");

	tea_caption_state_destroy(st);
}

static void test_stable_ignored_in_partial_mode(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_on_stable(st, "s", "a", 0, 1, "穩定", "open");
	expect_render(st, "", "transcript.stable is ignored in partial mode");
	tea_caption_state_on_partial(st, "s", "a", 1, "預覽");
	expect_render(st, "預覽", "partial mode still replaces the preview line");
	tea_caption_state_set_stable_mode(st, true);
	expect_render(st, "", "switching to stable mode drops the partial preview");
	tea_caption_state_set_stable_mode(st, false);
	tea_caption_state_on_final(st, "s", "a", 2, "完成");
	expect_render(st, "完成", "partial mode finals still render");
	tea_caption_state_destroy(st);
}

int main(void)
{
	tea_caption_state_t *state = tea_caption_state_create();
	expect(state != NULL, "caption state must be constructible without OBS");
	expect_render(state, "", "empty state must render no status text");

	tea_caption_state_on_partial(state, "session-1", "segment-1", 1, "partial");
	expect_render(state, "partial", "partial transcript must render");

	/* A stale partial is ignored, while a newer partial replaces the same
	 * segment. */
	tea_caption_state_on_partial(state, "session-1", "segment-1", 1, "stale");
	expect_render(state, "partial", "duplicate partial must be ignored");
	tea_caption_state_on_partial(state, "session-1", "segment-1", 2, "updated partial");
	expect_render(state, "updated partial", "newer partial must replace the preview");

	tea_caption_state_on_final(state, "session-1", "segment-1", 3, "final");
	expect_render(state, "final", "final transcript must replace its preview");
	tea_caption_state_on_partial(state, "session-1", "segment-1", 4, "must stay final");
	expect_render(state, "final", "partial after final must be ignored");

	/* Default logical window is final + current preview line. */
	tea_caption_state_on_partial(state, "session-1", "segment-2", 1, "next partial");
	expect_render(state, "final\nnext partial", "final plus preview must be retained");
	tea_caption_state_on_final(state, "session-1", "segment-2", 2, "next final");
	expect_render(state, "next final", "finalizing the preview advances the logical window");
	tea_caption_state_set_max_lines(state, 1);
	expect_render(state, "next final", "reducing max lines must trim the oldest final");

	tea_caption_state_reset(state);
	expect_render(state, "", "reset must clear transcript text");
	tea_caption_state_destroy(state);

	test_stable_append_only();
	test_stable_segment_isolation();
	test_stable_closing_states();
	test_stable_rejects_non_extension();
	test_stable_utf8_tails();
	test_stable_hold_for_reconnect();
	test_stable_ignored_in_partial_mode();

	puts("caption state tests passed");
	return EXIT_SUCCESS;
}
