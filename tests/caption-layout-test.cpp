#include "caption-layout.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(EXIT_FAILURE);
	}
}

} // namespace

int main()
{
	const tea_caption_layout_t fixed = tea_caption_layout_resolve(TEA_LAYOUT_MODE_FIXED, 960, 10, 120);
	expect(fixed.mode == TEA_LAYOUT_MODE_FIXED, "fixed mode must remain fixed");
	expect(fixed.outer_width == 960, "fixed outer width must be stable");
	expect(fixed.content_width == 940, "fixed content width must subtract both paddings");
	expect(fixed.force_word_wrap, "fixed mode must force native word wrapping");

	const tea_caption_layout_t fixed_again = tea_caption_layout_resolve(TEA_LAYOUT_MODE_FIXED, 960, 10, 2400);
	expect(fixed_again.outer_width == fixed.outer_width,
	       "fixed outer width must not depend on the child text width");
	expect(fixed_again.content_width == fixed.content_width,
	       "fixed content width must not depend on the child text width");

	const tea_caption_layout_t clamped = tea_caption_layout_resolve(TEA_LAYOUT_MODE_FIXED, 100, 10, 0);
	expect(clamped.outer_width == TEA_LAYOUT_DEFAULT_WIDTH_PX,
	       "invalid fixed width must use the documented default");
	expect(clamped.content_width == TEA_LAYOUT_DEFAULT_WIDTH_PX - 20,
	       "default fixed width must still account for padding");

	const tea_caption_layout_t auto_layout = tea_caption_layout_resolve(TEA_LAYOUT_MODE_AUTO, 960, 10, 640);
	expect(auto_layout.mode == TEA_LAYOUT_MODE_AUTO, "auto mode must remain auto");
	expect(auto_layout.outer_width == 660, "auto mode must preserve child width plus padding");
	expect(auto_layout.content_width == 640, "auto mode must preserve legacy child width");
	expect(!auto_layout.force_word_wrap, "auto mode must not override legacy word-wrap settings");

	const tea_caption_layout_t invalid_mode = tea_caption_layout_resolve(99, 960, 10, 640);
	expect(invalid_mode.mode == TEA_LAYOUT_MODE_AUTO, "unknown mode must fail safe to auto");

	/* Settings migration: old scene JSON has neither the new schema marker nor
	 * an explicit layout_mode and must retain the legacy auto-sized behavior.
	 * New sources and explicit user choices carry a marker/value and resolve to
	 * the requested mode. */
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_FIXED, false, false) == TEA_LAYOUT_MODE_AUTO,
	       "legacy scenes without a marker must remain auto-sized");
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_FIXED, true, false) == TEA_LAYOUT_MODE_FIXED,
	       "marked new sources must preserve fixed mode");
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_AUTO, true, false) == TEA_LAYOUT_MODE_AUTO,
	       "marked sources may explicitly choose auto mode");
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_FIXED, false, true) == TEA_LAYOUT_MODE_FIXED,
	       "an old scene may opt into fixed mode explicitly");

	expect(std::string(tea_caption_select_display_text("final transcript", true, "Waiting")) == "final transcript",
	       "transcript must always win over the placeholder");
	expect(std::string(tea_caption_select_display_text("", true, "Waiting")) == "Waiting",
	       "empty transcript may use the neutral placeholder");
	expect(std::string(tea_caption_select_display_text("", false, "Waiting")).empty(),
	       "placeholder can be disabled without changing transcript state");
	expect(std::string(tea_caption_select_display_text(nullptr, true, nullptr)).empty(),
	       "missing transcript and placeholder must render empty");

	std::cout << "caption layout tests passed\n";
	return EXIT_SUCCESS;
}
