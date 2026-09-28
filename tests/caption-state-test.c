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

	/* diverged: the final rewrote a committed character; the screen keeps
	 * the committed text and gets the final's part after it right away. */
	tea_caption_state_on_stable(st, "s", "div", 0, 1, "今天天氣", "open");
	tea_caption_state_on_final_indexed(st, "s", "div", 0, 4, "今天天汽很好");
	expect_render(st, "今天天氣很好", "a non-extending final never rewrites committed text, its rest follows");
	expect(tea_caption_state_last_close(st) == 3, "the final's remainder was taken");
	expect(!tea_caption_state_last_line_is_partial(st), "the line closes at the final");
	tea_caption_state_on_stable(st, "s", "div", 0, 2, "今天天氣很好", "diverged");
	expect_render(st, "今天天氣很好", "the closing diverged stable changes nothing");
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

/* ---------------- per-line snapshot + unstable tail ---------------- */

static const tea_caption_snapshot_line_t *snap_line(const tea_caption_snapshot_t *snap, int i)
{
	expect(i >= 0 && i < snap->count, "snapshot line index in range");
	return &snap->lines[i];
}

static void expect_tail(tea_caption_state_t *st, const char *text, const char *tail, const char *message)
{
	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count >= 1, message);
	const tea_caption_snapshot_line_t *last = snap_line(&snap, snap.count - 1);
	expect(strcmp(last->text, text) == 0, message);
	if (tail)
		expect(last->tail && strcmp(last->tail, tail) == 0, message);
	else
		expect(last->tail == NULL, message);
	tea_caption_snapshot_free(&snap);
}

static void test_snapshot_matches_render(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_max_lines(st, 3);
	tea_caption_state_on_final(st, "s", "a", 1, "first");
	tea_caption_state_on_partial(st, "s", "b", 1, "preview");
	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 2, "partial mode: finals plus the preview");
	expect(strcmp(snap_line(&snap, 0)->text, "first") == 0 && !snap_line(&snap, 0)->open, "final line first");
	expect(strcmp(snap_line(&snap, 1)->text, "preview") == 0 && snap_line(&snap, 1)->open, "preview line last");
	expect(snap_line(&snap, 1)->tail == NULL, "partial mode never reports a tail");
	uint64_t preview_key = snap_line(&snap, 1)->key;
	expect(preview_key != 0 && preview_key != snap_line(&snap, 0)->key, "lines have distinct keys");
	tea_caption_snapshot_free(&snap);

	uint64_t rev = tea_caption_state_revision(st);
	tea_caption_state_on_partial(st, "s", "b", 2, "preview grows");
	expect(tea_caption_state_revision(st) > rev, "a change bumps the revision");
	tea_caption_state_on_final(st, "s", "b", 3, "preview grows final");
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 2 && snap_line(&snap, 1)->key == preview_key,
	       "the final keeps the preview line's identity (no flash on screen)");
	tea_caption_snapshot_free(&snap);
	tea_caption_state_destroy(st);
}

static void test_unstable_tail(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_set_stable_tail_lines(st, true); /* tails shown */

	tea_caption_state_on_stable(st, "s", "seg-a", 0, 1, "我們", "open");
	tea_caption_state_on_partial(st, "s", "seg-a", 2, "我們需要");
	expect_tail(st, "我們", "需要", "a partial extending the committed text shows its extra part as tail");
	expect_render(st, "我們", "render() never shows the tail");

	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(st, false, &snap);
	expect(snap.count == 1 && snap_line(&snap, 0)->tail == NULL, "tail display off: no tail reported");
	uint64_t key = snap_line(&snap, 0)->key;
	tea_caption_snapshot_free(&snap);

	/* Shown text never retracts. A partial that does not start with the
	 * committed text offers what it has after as many characters as the
	 * committed text (how the server closes a diverged segment). */
	tea_caption_state_on_partial(st, "s", "seg-a", 3, "你們需要改");
	expect_tail(st, "我們", "需要改", "a non-prefix partial offers its text after the committed length");
	expect_render(st, "我們", "and never changes the committed text");

	tea_caption_state_on_partial(st, "s", "seg-a", 4, "我們需要改");
	expect_tail(st, "我們", "需要改", "the same tail again changes nothing");
	tea_caption_state_on_partial(st, "s", "seg-a", 5, "我們需");
	expect_tail(st, "我們", "需要改", "a shorter offer does not retract the shown tail");
	tea_caption_state_on_partial(st, "s", "seg-a", 6, "我們");
	expect_tail(st, "我們", "需要改", "a partial with nothing beyond the committed text does not retract");
	tea_caption_state_on_partial(st, "s", "seg-a", 7, "他們");
	expect_tail(st, "我們", "需要改", "a short non-prefix partial does not retract either");

	tea_caption_state_on_stable(st, "s", "seg-a", 0, 2, "我們需", "open");
	expect_tail(st, "我們需", "要改",
		    "committing part of the tail moves it into the text, nothing visible changes");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 3, "我們需要改", "open");
	expect_tail(st, "我們需要改", NULL, "no tail once everything is committed");
	tea_caption_state_on_partial(st, "s", "seg-a", 8, "我們需要改一");
	expect_tail(st, "我們需要改", "一", "a later partial brings a new tail");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 4, "我們需要改二", "open");
	expect_tail(st, "我們需要改二", NULL, "a commit that contradicts the tail drops it; committed text wins");
	tea_caption_state_on_partial(st, "s", "seg-a", 9, "我們需要改二三");
	tea_caption_state_on_partial(st, "s", "seg-a", 10, "我們需要改二四");
	expect_tail(st, "我們需要改二", "四", "an offer as long as the tail may correct its words");
	tea_caption_state_on_partial(st, "s", "seg-a", 11, "我們需要改二四五。");
	expect_tail(st, "我們需要改二", "四五",
		    "the hypothesis' closing punctuation is not shown at the unstable edge");
	tea_caption_state_on_partial(st, "s", "seg-a", 12, "我們需要改二四五六。");
	expect_tail(st, "我們需要改二", "四五六", "so the next preview only appends");

	tea_caption_state_on_final(st, "s", "seg-a", 13, "我們需要改二四五六。");
	expect_tail(st, "我們需要改二四五六。", NULL, "a final that extends what is shown becomes the committed text");
	tea_caption_state_on_partial(st, "s", "seg-a", 14, "我們需要改二四五六。了");
	expect_tail(st, "我們需要改二四五六。", NULL, "no tail after the final");

	tea_caption_state_snapshot(st, true, &snap);
	expect(snap_line(&snap, 0)->key == key, "the line keeps its identity while it grows");
	tea_caption_snapshot_free(&snap);

	/* a closing stable that does not extend what is shown never takes it back */
	tea_caption_state_on_stable(st, "s", "seg-b", 1, 1, "下一句", "open");
	tea_caption_state_on_partial(st, "s", "seg-b", 1, "下一句話");
	expect_tail(st, "下一句", "話", "second segment shows its own tail");
	tea_caption_state_on_stable(st, "s", "seg-b", 1, 2, "下一句", "diverged");
	expect_tail(st, "下一句", "話", "a closing stable shorter than what is shown keeps the shown tail");
	expect(!tea_caption_state_last_line_is_partial(st), "the line is closed");

	/* a final that differs replaces the tail with its own part after the
	 * committed text; the closing "diverged" stable behind it changes nothing */
	tea_caption_state_on_stable(st, "s", "seg-e", 4, 1, "第五句", "open");
	tea_caption_state_on_partial(st, "s", "seg-e", 1, "第五句話");
	tea_caption_state_on_final(st, "s", "seg-e", 2, "第五個問題");
	expect_tail(st, "第五句問題", NULL,
		    "a final that differs replaces the unconfirmed tail with its own remainder; committed text stays");
	expect(tea_caption_state_last_close(st) == 3, "the final was taken");
	expect(!tea_caption_state_last_line_is_partial(st), "the line closes");
	tea_caption_state_on_stable(st, "s", "seg-e", 4, 2, "第五句個問題", "diverged");
	expect_tail(st, "第五句問題", NULL, "the diverged closing stable changes nothing");
	/* the real-model case: the final drops the phrase the tail showed */
	tea_caption_state_on_stable(st, "s", "seg-f", 5, 1, "醉人的芬芳。", "open");
	tea_caption_state_on_partial(st, "s", "seg-f", 1, "醉人的芬芳。步伐踉蹌，險些跌倒在地。");
	tea_caption_state_on_final(st, "s", "seg-f", 2, "醉人的芬芳。");
	tea_caption_state_on_stable(st, "s", "seg-f", 5, 2, "醉人的芬芳。", "final");
	expect_tail(st, "醉人的芬芳。", "步伐踉蹌，險些跌倒在地",
		    "a final shorter than what is shown keeps the phrase the viewer already read");
	expect(tea_caption_state_last_close(st) == 2, "phrase drop: the shown tail was kept");
	/* the e2e stable_tail case: the audio ends, the last partial falls back to
	 * the committed text and the final is the shown text minus its end */
	tea_caption_state_on_stable(st, "s", "seg-h", 5, 1, "然後喝", "open");
	tea_caption_state_on_partial(st, "s", "seg-h", 1, "然後喝杯茶啊");
	tea_caption_state_on_partial(st, "s", "seg-h", 2, "然後喝");
	tea_caption_state_on_final(st, "s", "seg-h", 3, "然後喝");
	expect_tail(st, "然後喝", "杯茶啊", "a truncated final never retracts the shown tail");
	expect(tea_caption_state_last_close(st) == 2, "truncation: the shown tail was kept");
	/* the fake backend's final swaps the last committed character for a
	 * filler: its remainder after the committed text is empty, so taking it
	 * would only cut the tail off */
	tea_caption_state_on_stable(st, "s", "seg-i", 5, 1, "然後喝", "open");
	tea_caption_state_on_partial(st, "s", "seg-i", 1, "然後喝杯茶嗯");
	tea_caption_state_on_final(st, "s", "seg-i", 2, "然後啊");
	expect_tail(st, "然後喝", "杯茶嗯", "a final adding nothing after the committed text keeps the tail");
	expect(tea_caption_state_last_close(st) == 2, "nothing to take: the shown tail was kept");
	/* a closing stable that extends what is shown is shown */
	tea_caption_state_on_stable(st, "s", "seg-g", 6, 1, "好的", "open");
	tea_caption_state_on_partial(st, "s", "seg-g", 1, "好的謝謝");
	tea_caption_state_on_final(st, "s", "seg-g", 2, "好的謝謝大家。");
	expect_tail(st, "好的謝謝大家。", NULL, "a final extending committed + tail replaces the tail seamlessly");

	/* skipped / cancelled / reconnect end the tail too */
	tea_caption_state_on_stable(st, "s", "seg-c", 7, 1, "第三", "open");
	tea_caption_state_on_partial(st, "s", "seg-c", 1, "第三句");
	tea_caption_state_on_segment_dropped(st, "s", "seg-c");
	expect_tail(st, "第三", "句", "a dropped segment keeps what it shows, tail included");
	tea_caption_state_on_stable(st, "s", "seg-d", 8, 1, "第四", "open");
	tea_caption_state_on_partial(st, "s", "seg-d", 1, "第四句");
	tea_caption_state_hold_for_reconnect(st);
	expect_tail(st, "第四", "句", "a connection loss freezes what is shown, tail included");

	expect(tea_caption_state_stable_mismatches(st) == 0, "tails never count as contract violations");
	tea_caption_state_destroy(st);
}

static void test_close_with_tails_hidden(void)
{
	/* Tails not shown: what is shown is the committed text only; a final
	 * that rewrites a committed character keeps the committed text and adds
	 * its own part after it. */
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 1, "第五句", "open");
	tea_caption_state_on_partial(st, "s", "seg-a", 1, "第五句話");
	tea_caption_state_on_final(st, "s", "seg-a", 2, "第五個問題。");
	expect_render(st, "第五句問題。", "committed text kept, the final's part after it follows");
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 2, "第五句問題。", "diverged");
	expect_render(st, "第五句問題。", "the diverged closing stable changes nothing");
	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 1 && snap.lines[0].tail == NULL, "no hidden tail is kept after the close");
	tea_caption_snapshot_free(&snap);
	tea_caption_state_destroy(st);
}

static void test_tail_only_lines(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 1, "第一句", "final");

	tea_caption_state_on_partial(st, "s", "seg-b", 1, "還沒");
	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 1, "without tail lines a segment needs committed text to get a line");
	tea_caption_snapshot_free(&snap);

	tea_caption_state_set_stable_tail_lines(st, true);
	tea_caption_state_on_partial(st, "s", "seg-c", 1, "預覽");
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 2, "with tail lines the first partial gets its own line");
	expect(strcmp(snap_line(&snap, 1)->text, "") == 0 && snap_line(&snap, 1)->tail &&
		       strcmp(snap_line(&snap, 1)->tail, "預覽") == 0,
	       "a tail-only line has no committed text");
	uint64_t key = snap_line(&snap, 1)->key;
	tea_caption_snapshot_free(&snap);
	expect_render(st, "第一句", "render() still shows committed text only");
	tea_caption_state_snapshot(st, false, &snap);
	expect(snap.count == 1, "tail display off hides tail-only lines");
	tea_caption_snapshot_free(&snap);

	tea_caption_state_on_stable(st, "s", "seg-c", 2, 1, "預", "open");
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 2 && snap_line(&snap, 1)->key == key && strcmp(snap_line(&snap, 1)->text, "預") == 0 &&
		       snap_line(&snap, 1)->tail && strcmp(snap_line(&snap, 1)->tail, "覽") == 0,
	       "the first stable fills the same line");
	tea_caption_snapshot_free(&snap);
	tea_caption_state_destroy(st);
}

static void test_activity(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 1, "我們", "open");
	tea_caption_snapshot_t a, b;
	tea_caption_state_snapshot(st, true, &a);
	/* a partial that changes nothing visible is still speech activity */
	tea_caption_state_on_partial(st, "s", "seg-a", 1, "你們");
	tea_caption_state_snapshot(st, true, &b);
	expect(strcmp(b.lines[0].text, "我們") == 0 && b.lines[0].tail == NULL, "nothing visible changed");
	expect(b.lines[0].activity != a.lines[0].activity, "the line's activity changed");
	expect(b.activity != a.activity, "the session's activity changed");
	tea_caption_snapshot_free(&a);
	tea_caption_snapshot_free(&b);
	tea_caption_state_snapshot(st, true, &a);
	tea_caption_state_on_stable(st, "s", "seg-a", 0, 1, "我們", "open"); /* duplicate */
	tea_caption_state_snapshot(st, true, &b);
	expect(b.activity != a.activity, "even a duplicate stable shows the session is active");
	tea_caption_snapshot_free(&a);
	tea_caption_snapshot_free(&b);
	tea_caption_state_destroy(st);
}

/* ---------------- real sequences from a user's live trace ----------------
 * tea-trace-20260928-080614-b1636511.jsonl (300 ms preview cadence, end
 * silence 300 ms), segments 4, 7, 8, 14 and 15, event for event. */

#include "caption-align.h"

enum { TEA_EV_P, TEA_EV_S, TEA_EV_F };
typedef struct {
	int kind;
	uint64_t rev;
	const char *text;
	const char *state;
} tea_trace_ev_t;

/* Plays a segment; after every event checks that the shown text never
 * repeats a 4+ character run more often than the segment's final has it.
 * Returns the shown text (committed + tail) after the final, bfree()-able. */
static char *play_segment(const tea_trace_ev_t *evs, size_t n, const char *final_text, const char *name)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_set_stable_tail_lines(st, true);
	tea_norm_t fin;
	tea_norm_build(final_text, strlen(final_text), &fin);
	for (size_t i = 0; i < n; i++) {
		const tea_trace_ev_t *e = &evs[i];
		if (e->kind == TEA_EV_P)
			tea_caption_state_on_partial(st, "s", name, e->rev, e->text);
		else if (e->kind == TEA_EV_S)
			tea_caption_state_on_stable(st, "s", name, 0, e->rev, e->text, e->state);
		else
			tea_caption_state_on_final_indexed(st, "s", name, 0, e->rev, e->text);
		tea_caption_snapshot_t snap;
		tea_caption_state_snapshot(st, true, &snap);
		if (snap.count == 1) {
			char shown[4096];
			snprintf(shown, sizeof(shown), "%s%s", snap.lines[0].text,
				 snap.lines[0].tail ? snap.lines[0].tail : "");
			tea_norm_t norm;
			tea_norm_build(shown, strlen(shown), &norm);
			if (tea_norm_has_new_repeat(&norm, &fin, 4)) {
				fprintf(stderr, "%s event %zu shows [%s]\n", name, i, shown);
				expect(false, "a real sequence never shows a phrase twice");
			}
		}
		tea_caption_snapshot_free(&snap);
	}
	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(st, true, &snap);
	expect(snap.count == 1, "one line");
	char *shown = bmalloc(strlen(snap.lines[0].text) + (snap.lines[0].tail ? strlen(snap.lines[0].tail) : 0) + 1);
	strcpy(shown, snap.lines[0].text);
	if (snap.lines[0].tail)
		strcat(shown, snap.lines[0].tail);
	tea_caption_snapshot_free(&snap);
	tea_caption_state_destroy(st);
	return shown;
}
static const tea_trace_ev_t k_seg4[] = {
	{TEA_EV_P, 1, "對。", NULL},
	{TEA_EV_P, 2, "對因為", NULL},
	{TEA_EV_S, 1, "對", "open"},
	{TEA_EV_P, 3, "對，因爲我我", NULL},
	{TEA_EV_P, 4, "對因為我我今天", NULL},
	{TEA_EV_P, 5, "對因為我我今天也有跟", NULL},
	{TEA_EV_S, 2, "對因為我我今天", "open"},
	{TEA_EV_P, 6, "對因為我我今天也有跟大家聊。", NULL},
	{TEA_EV_S, 3, "對因為我我今天也有跟", "open"},
	{TEA_EV_P, 7, "對因為我我今天也跟大家聊一聊。", NULL},
	{TEA_EV_P, 8, "對因為我我今天也跟大家聊一聊。", NULL},
	{TEA_EV_P, 9, "對因為我我今天也有跟大家聊一聊然後。", NULL},
	{TEA_EV_P, 10, "對因為我我今天也有跟大家聊一聊然後你也有。", NULL},
	{TEA_EV_S, 4, "對因為我我今天也有跟大家聊一聊然後", "open"},
	{TEA_EV_P, 11, "對因為我我今天也有跟大家聊一聊然後你也有談到。", NULL},
	{TEA_EV_S, 5, "對因為我我今天也有跟大家聊一聊然後你也有", "open"},
	{TEA_EV_P, 12, "對因為我我今天也有跟大家聊一聊然後你也有談到說那個。", NULL},
	{TEA_EV_S, 6, "對因為我我今天也有跟大家聊一聊然後你也有談到", "open"},
	{TEA_EV_P, 13, "對因為我我今天也有跟大家聊一聊然後你也有談到說那個。", NULL},
	{TEA_EV_S, 7, "對因為我我今天也有跟大家聊一聊然後你也有談到說那個", "open"},
	{TEA_EV_P, 14, "對，因為我我今天也有跟大家聊一聊，然後你也有談到說那個呃。", NULL},
	{TEA_EV_P, 15, "對因為我我今天也有跟大家聊一聊然後你也有談到說那個呃你本來。", NULL},
	{TEA_EV_F, 16, "對因為我我今天也有跟大家聊一聊然後你也有談到說那個呃你本來跟", NULL},
	{TEA_EV_S, 8, "對因為我我今天也有跟大家聊一聊然後你也有談到說那個呃你本來跟", "final"},
};
static const tea_trace_ev_t k_seg7[] = {
	{TEA_EV_P, 1, "後來。", NULL},
	{TEA_EV_P, 2, "後來在這。", NULL},
	{TEA_EV_S, 1, "後來", "open"},
	{TEA_EV_P, 3, "後來在這三個禮拜", NULL},
	{TEA_EV_S, 2, "後來在這", "open"},
	{TEA_EV_P, 4, "後來在這三個禮拜。", NULL},
	{TEA_EV_S, 3, "後來在這三個禮拜", "open"},
	{TEA_EV_P, 5, "後來在這三個禮拜突然", NULL},
	{TEA_EV_P, 6, "後來在這三個禮拜突然。", NULL},
	{TEA_EV_S, 4, "後來在這三個禮拜突然", "open"},
	{TEA_EV_P, 7, "後來在這三個禮拜突然找到。", NULL},
	{TEA_EV_P, 8, "後來在這三個禮拜突然找到你人生。", NULL},
	{TEA_EV_S, 5, "後來在這三個禮拜突然找到", "open"},
	{TEA_EV_P, 9, "後來在這三個禮拜突然找到你人生可以。", NULL},
	{TEA_EV_S, 6, "後來在這三個禮拜突然找到你人生", "open"},
	{TEA_EV_P, 10, "後來在這三個禮拜突然找到你人生可以做的一些事情。", NULL},
	{TEA_EV_S, 7, "後來在這三個禮拜突然找到你人生可以", "open"},
	{TEA_EV_P, 11, "後來在這三個禮拜突然找到你人生可以做的事。", NULL},
	{TEA_EV_S, 8, "後來在這三個禮拜突然找到你人生可以做的", "open"},
	{TEA_EV_P, 12, "後來在這三個禮拜突然找到你人生可以做的勢力點。", NULL},
	{TEA_EV_F, 13, "後來在這三個禮拜突然找到你人生可以做的勢力點。", NULL},
	{TEA_EV_S, 9, "後來在這三個禮拜突然找到你人生可以做的勢力點。", "final"},
};
static const tea_trace_ev_t k_seg8[] = {
	{TEA_EV_P, 1, "比較。", NULL},     {TEA_EV_P, 2, "比較", NULL},   {TEA_EV_S, 1, "比較", "open"},
	{TEA_EV_P, 3, "對對對", NULL},     {TEA_EV_P, 4, "對對對", NULL}, {TEA_EV_F, 5, "因為", NULL},
	{TEA_EV_S, 2, "比較", "diverged"},
};
static const tea_trace_ev_t k_seg14[] = {
	{TEA_EV_P, 1, "啊，給。", NULL},
	{TEA_EV_P, 2, "okay拜拜收工", NULL},
	{TEA_EV_P, 3, "okaybyebye so byebye so", NULL},
	{TEA_EV_F, 4, "啊，可以，快快走，快快走。", NULL},
	{TEA_EV_S, 1, "啊，可以，快快走，快快走。", "final"},
};
static const tea_trace_ev_t k_seg15[] = {
	{TEA_EV_P, 1, "真正", NULL},
	{TEA_EV_P, 2, "真正好的。", NULL},
	{TEA_EV_S, 1, "真正", "open"},
	{TEA_EV_P, 3, "真正好的教育。", NULL},
	{TEA_EV_S, 2, "真正好的", "open"},
	{TEA_EV_P, 4, "真正好的教育當中不", NULL},
	{TEA_EV_S, 3, "真正好的教育", "open"},
	{TEA_EV_P, 5, "真正好的教育當中不是讓", NULL},
	{TEA_EV_S, 4, "真正好的教育當中不", "open"},
	{TEA_EV_P, 6, "真正好的教育當中不是讓你", NULL},
	{TEA_EV_S, 5, "真正好的教育當中不是讓", "open"},
	{TEA_EV_P, 7, "真正好的教育當中不是讓你不學習。", NULL},
	{TEA_EV_S, 6, "真正好的教育當中不是讓你", "open"},
	{TEA_EV_P, 8, "真正好的教育當中不是讓你不需要別人。", NULL},
	{TEA_EV_S, 7, "真正好的教育當中不是讓你不", "open"},
	{TEA_EV_P, 9, "真正好的教育當中，不是讓你不需要別人。", NULL},
	{TEA_EV_F, 10, "真正好的教育當中不是讓你不需要別人。", NULL},
	{TEA_EV_S, 8, "真正好的教育當中不是讓你不需要別人。", "final"},
};

#define TEA_N(a) (sizeof(a) / sizeof((a)[0]))

static void test_real_trace_sequences(void)
{
	/* seg 4: the partials restate the committed text with commas added;
	 * matched ignoring punctuation, nothing is shown twice, and the final
	 * (which continues the committed text) is what the line ends with. */
	char *s4 = play_segment(k_seg4, TEA_N(k_seg4), k_seg4[TEA_N(k_seg4) - 2].text, "seg4");
	expect(strcmp(s4, k_seg4[TEA_N(k_seg4) - 2].text) == 0, "seg 4 ends with its final, once");
	bfree(s4);

	/* seg 7: the dim tail said 一些事情, the final 勢力點: the final wins */
	char *s7 = play_segment(k_seg7, TEA_N(k_seg7), k_seg7[TEA_N(k_seg7) - 2].text, "seg7");
	expect(strcmp(s7, "後來在這三個禮拜突然找到你人生可以做的勢力點。") == 0,
	       "seg 7: the final replaces the dim tail");
	bfree(s7);

	/* seg 8: committed 比較, dim 對對對, final 因為 (does not contain the
	 * committed text): committed text stays, the final follows it */
	char *s8 = play_segment(k_seg8, TEA_N(k_seg8), k_seg8[TEA_N(k_seg8) - 2].text, "seg8");
	expect(strcmp(s8, "比較因為") == 0, "seg 8: committed text kept, the final replaces the dim tail");
	bfree(s8);

	/* seg 14: no committed text, dim "okaybyebye so byebye so", a different final */
	char *s14 = play_segment(k_seg14, TEA_N(k_seg14), "okaybyebye so byebye so", "seg14");
	expect(strcmp(s14, "啊，可以，快快走，快快走。") == 0, "seg 14: the final replaces an unrelated dim tail");
	bfree(s14);

	/* seg 15: a comma inside a restatement used to duplicate the sentence */
	char *s15 = play_segment(k_seg15, TEA_N(k_seg15), k_seg15[TEA_N(k_seg15) - 2].text, "seg15");
	expect(strcmp(s15, "真正好的教育當中不是讓你不需要別人。") == 0, "seg 15 ends with its final, once");
	bfree(s15);
}

/* The two plugin-side duplications the church soak (round 6) found, rebuilt
 * with neutral text: the shapes are those of the private traces. */
static const tea_trace_ev_t k_window_moved[] = {
	/* the committed text ends in 我們; the next preview starts there */
	{TEA_EV_P, 1, "好的，我們", NULL},
	{TEA_EV_S, 1, "好的", "open"},
	{TEA_EV_P, 2, "好的，我們。", NULL},
	{TEA_EV_S, 2, "好的，我們", "open"},
	{TEA_EV_P, 3, "我們開始", NULL},
	{TEA_EV_P, 4, "我們開始今天的聚會。", NULL},
	/* and the final starts two characters before the committed end's run */
	{TEA_EV_F, 5, "是的我們開始今天的聚會。", NULL},
	{TEA_EV_S, 3, "好的，我們開始今天的聚會。", "diverged"},
};

static const tea_trace_ev_t k_restated_end[] = {
	/* committed 大家跟旁邊的朋友; the model restates it, the last two
	 * characters and the first ones heard differently */
	{TEA_EV_P, 1, "大家跟旁邊的朋友。", NULL},
	{TEA_EV_S, 1, "大家跟旁邊的朋友", "open"},
	{TEA_EV_P, 2, "大家跟你們旁邊來，一起一起，唱詩歌。", NULL},
	{TEA_EV_P, 3, "他家跟旁邊的彭友，一起唱詩歌。", NULL},
	{TEA_EV_P, 4, "打架跟旁邊的彭友，一起唱詩歌讚美。", NULL},
	{TEA_EV_F, 5, "打架跟旁邊的彭友，一起唱詩歌讚美。", NULL},
	{TEA_EV_S, 2, "大家跟旁邊的朋友，一起唱詩歌讚美。", "diverged"},
};

static void test_restatement_sequences(void)
{
	char *a = play_segment(k_window_moved, TEA_N(k_window_moved), "是的我們開始今天的聚會。", "moved");
	expect(strcmp(a, "好的，我們開始今天的聚會。") == 0,
	       "a preview starting at the committed end continues it; so does a final shifted before it");
	bfree(a);
	char *b = play_segment(k_restated_end, TEA_N(k_restated_end), "打架跟旁邊的彭友，一起唱詩歌讚美。", "restated");
	expect(strcmp(b, "大家跟旁邊的朋友，一起唱詩歌讚美。") == 0,
	       "a restatement of the committed end heard differently continues after it, never appended whole");
	bfree(b);

	/* the head rule: a preview that begins like the committed text but
	 * cannot be placed is not shown as new speech */
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_set_stable_tail_lines(st, true);
	tea_caption_state_on_stable(st, "s", "h", 1, 1, "大家跟旁邊的朋友", "open");
	tea_caption_state_on_partial(st, "s", "h", 1, "大家跟你們旁邊來，一起一起，唱詩歌。");
	expect_tail(st, "大家跟旁邊的朋友", NULL, "a whole-sentence restatement heard differently is not appended");
	tea_caption_state_destroy(st);
}

static void test_audio_ack_progress(void)
{
	tea_caption_state_t *st = tea_caption_state_create();
	tea_caption_state_set_stable_mode(st, true);
	tea_caption_state_on_stable(st, "s", "a", 1, 1, "還在說", "open");
	const uint64_t rev = tea_caption_state_revision(st);
	const uint64_t p0 = tea_caption_state_server_progress(st);
	tea_caption_state_on_audio_ack(st, 1600);
	const uint64_t p1 = tea_caption_state_server_progress(st);
	expect(p1 != p0, "an ack that moves the acknowledged position is progress");
	tea_caption_state_on_audio_ack(st, 1600);
	expect(tea_caption_state_server_progress(st) == p1, "the same position again is not");
	tea_caption_state_on_audio_ack(st, 3200);
	expect(tea_caption_state_server_progress(st) != p1, "a later position is");
	expect(tea_caption_state_revision(st) == rev, "acks never cause a re-render");
	expect(tea_caption_state_last_line_is_partial(st), "and never close the open line");
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
	test_restatement_sequences();
	test_audio_ack_progress();
	test_stable_segment_isolation();
	test_stable_closing_states();
	test_stable_rejects_non_extension();
	test_stable_utf8_tails();
	test_stable_hold_for_reconnect();
	test_stable_ignored_in_partial_mode();
	test_snapshot_matches_render();
	test_unstable_tail();
	test_tail_only_lines();
	test_close_with_tails_hidden();
	test_activity();
	test_real_trace_sequences();

	puts("caption state tests passed");
	return EXIT_SUCCESS;
}
