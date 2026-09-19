#include "caption-state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

	puts("caption state tests passed");
	return EXIT_SUCCESS;
}
