/*
 * Theme palettes (Sections 57-58, 77-78).
 *
 * Transcribed from the supplied theme-switcher reference. Each theme also gets
 * a slight vertical shift between bg_top and bg_bot: a gradient costs the
 * compositor exactly what a flat fill costs (one write per pixel, same as
 * gfx_clear), and it is what makes the alpha-tinted panes read as frosted
 * glass rather than as flat grey rectangles.
 */

#include <nexus/theme.h>
#include <zephyr/kernel.h>
#include <string.h>

static const struct nexus_theme themes[] = {
	{
		/* The reference "aurora glass" look. */
		.name = "NEXUS",
		.bg_top = NEXUS_C(0x0B0B1Eu), .bg_bot = NEXUS_C(0x05050Cu),
		.glow_a = NEXUS_C(0xD633FFu), .glow_b = NEXUS_C(0x00E0FFu),
		.glow_alpha = 22,
		.panel = NEXUS_C(0xBFD4FFu), .panel_alpha = 30,
		.edge_hi = NEXUS_C(0xFFFFFFu), .edge_hi_alpha = 92,
		.edge_lo = NEXUS_C(0x000814u), .edge_lo_alpha = 70,
		.border = NEXUS_C(0xFFFFFFu), .border_alpha = 38,
		.caption = NEXUS_C(0x8B93C4u), .value = NEXUS_C(0xFFFFFFu),
		.accent = NEXUS_C(0x5CFFC0u), .accent_alt = NEXUS_C(0xFF5FA2u),
		.muted = NEXUS_C(0x5B628Fu), .track = NEXUS_C(0x03030Au),
		.success = NEXUS_C(0x5CFFC0u), .warning = NEXUS_C(0xFFC24Du),
		.error = NEXUS_C(0xFF5F6Du),
		.wordmark = { NEXUS_C(0xFFFFFFu), NEXUS_C(0xFFC2DCu),
			      NEXUS_C(0xFF5FA2u), NEXUS_C(0x8E2A63u),
			      NEXUS_C(0x3A0F2Au) },
		.radius = 7,
	},
	{
		/* Same language, zero-emission ground: an OLED burns no power on
		 * true black, so the gradient and the glows both come off. */
		.name = "AMOLED",
		.bg_top = NEXUS_C(0x000000u), .bg_bot = NEXUS_C(0x000000u),
		.glow_a = NEXUS_C(0x000000u), .glow_b = NEXUS_C(0x000000u),
		.glow_alpha = 0,
		.panel = NEXUS_C(0x9FB4E0u), .panel_alpha = 22,
		.edge_hi = NEXUS_C(0xFFFFFFu), .edge_hi_alpha = 70,
		.edge_lo = NEXUS_C(0x000000u), .edge_lo_alpha = 90,
		.border = NEXUS_C(0xFFFFFFu), .border_alpha = 32,
		.caption = NEXUS_C(0x7A82B0u), .value = NEXUS_C(0xFFFFFFu),
		.accent = NEXUS_C(0x5CFFC0u), .accent_alt = NEXUS_C(0xFF5FA2u),
		.muted = NEXUS_C(0x4A5178u), .track = NEXUS_C(0x000000u),
		.success = NEXUS_C(0x5CFFC0u), .warning = NEXUS_C(0xFFC24Du),
		.error = NEXUS_C(0xFF5F6Du),
		.wordmark = { NEXUS_C(0xFFFFFFu), NEXUS_C(0xFFC2DCu),
			      NEXUS_C(0xFF5FA2u), NEXUS_C(0x8E2A63u),
			      NEXUS_C(0x3A0F2Au) },
		.radius = 7,
	},
	{
		/* Light theme: panes are brighter than the ground, so the shaded
		 * bottom edge does the work the highlight does elsewhere. */
		.name = "DAYLIGHT",
		.bg_top = NEXUS_C(0xEDF1F9u), .bg_bot = NEXUS_C(0xDCE3F1u),
		.glow_a = NEXUS_C(0x788CFFu), .glow_b = NEXUS_C(0xFF8CBEu),
		.glow_alpha = 26,
		.panel = NEXUS_C(0xFFFFFFu), .panel_alpha = 165,
		.edge_hi = NEXUS_C(0xFFFFFFu), .edge_hi_alpha = 230,
		.edge_lo = NEXUS_C(0x8C96B4u), .edge_lo_alpha = 70,
		.border = NEXUS_C(0xFFFFFFu), .border_alpha = 235,
		.caption = NEXUS_C(0x7B84AAu), .value = NEXUS_C(0x2B3350u),
		.accent = NEXUS_C(0x17A673u), .accent_alt = NEXUS_C(0x3B4BC8u),
		.muted = NEXUS_C(0x8990B0u), .track = NEXUS_C(0xC6CDE0u),
		.success = NEXUS_C(0x17A673u), .warning = NEXUS_C(0xC98A12u),
		.error = NEXUS_C(0xC8324Bu),
		.wordmark = { NEXUS_C(0x8FA0F0u), NEXUS_C(0x5A6BE0u),
			      NEXUS_C(0x3B4BC8u), NEXUS_C(0x26308Au),
			      NEXUS_C(0x161C52u) },
		.radius = 7,
	},
	{
		/* Neumorphic: panes are the same colour as the ground and read
		 * only through their lit and shaded edges (Section 58). */
		.name = "CLAY",
		.bg_top = NEXUS_C(0x2B3143u), .bg_bot = NEXUS_C(0x232835u),
		.glow_a = NEXUS_C(0x000000u), .glow_b = NEXUS_C(0x000000u),
		.glow_alpha = 0,
		.panel = NEXUS_C(0x272C3Bu), .panel_alpha = 255,
		.edge_hi = NEXUS_C(0x4A5271u), .edge_hi_alpha = 255,
		.edge_lo = NEXUS_C(0x151824u), .edge_lo_alpha = 255,
		.border = NEXUS_C(0x353B4Eu), .border_alpha = 200,
		.caption = NEXUS_C(0x79809Cu), .value = NEXUS_C(0xE4E9F7u),
		.accent = NEXUS_C(0x5FC79Bu), .accent_alt = NEXUS_C(0xE86A9Au),
		.muted = NEXUS_C(0x666E8Cu), .track = NEXUS_C(0x1D2130u),
		.success = NEXUS_C(0x5FC79Bu), .warning = NEXUS_C(0xE0A63Cu),
		.error = NEXUS_C(0xE8536Au),
		.wordmark = { NEXUS_C(0xF7C2D8u), NEXUS_C(0xE86A9Au),
			      NEXUS_C(0xA83C6Cu), NEXUS_C(0x6B2244u),
			      NEXUS_C(0x3A1226u) },
		.radius = 9,
	},
	{
		.name = "ESPRESSO",
		.bg_top = NEXUS_C(0x2C201Au), .bg_bot = NEXUS_C(0x1C1410u),
		.glow_a = NEXUS_C(0xE9A23Bu), .glow_b = NEXUS_C(0xC45C34u),
		.glow_alpha = 20,
		.panel = NEXUS_C(0xFFE4C4u), .panel_alpha = 24,
		.edge_hi = NEXUS_C(0xFFEBD2u), .edge_hi_alpha = 80,
		.edge_lo = NEXUS_C(0x120C08u), .edge_lo_alpha = 80,
		.border = NEXUS_C(0xFFDAB0u), .border_alpha = 43,
		.caption = NEXUS_C(0xA38872u), .value = NEXUS_C(0xF7E9D8u),
		.accent = NEXUS_C(0xE9A23Bu), .accent_alt = NEXUS_C(0xFFD9A0u),
		.muted = NEXUS_C(0x7E6952u), .track = NEXUS_C(0x14100Cu),
		.success = NEXUS_C(0x9BC46Au), .warning = NEXUS_C(0xE9A23Bu),
		.error = NEXUS_C(0xD9553Fu),
		.wordmark = { NEXUS_C(0xFFF0D8u), NEXUS_C(0xFFD9A0u),
			      NEXUS_C(0xE9A23Bu), NEXUS_C(0x8A5414u),
			      NEXUS_C(0x4A2C0Au) },
		.radius = 7,
	},
	{
		.name = "MINT",
		.bg_top = NEXUS_C(0x07170Fu), .bg_bot = NEXUS_C(0x030B07u),
		.glow_a = NEXUS_C(0x5CFFB0u), .glow_b = NEXUS_C(0x3CC8FFu),
		.glow_alpha = 14,
		.panel = NEXUS_C(0x7CFFC4u), .panel_alpha = 18,
		.edge_hi = NEXUS_C(0xC4FFE4u), .edge_hi_alpha = 78,
		.edge_lo = NEXUS_C(0x000603u), .edge_lo_alpha = 80,
		.border = NEXUS_C(0x7CFFC4u), .border_alpha = 51,
		.caption = NEXUS_C(0x3E7E64u), .value = NEXUS_C(0xD8FFEEu),
		.accent = NEXUS_C(0x5CFFB0u), .accent_alt = NEXUS_C(0xC4FFE4u),
		.muted = NEXUS_C(0x2F6B54u), .track = NEXUS_C(0x020805u),
		.success = NEXUS_C(0x5CFFB0u), .warning = NEXUS_C(0xD8D35Cu),
		.error = NEXUS_C(0xFF6B7Au),
		.wordmark = { NEXUS_C(0xE8FFF4u), NEXUS_C(0xB8FFDCu),
			      NEXUS_C(0x5CFFB0u), NEXUS_C(0x2FBE80u),
			      NEXUS_C(0x0E4A32u) },
		.radius = 7,
	},
	{
		/* The full-height gradient the reference leans hardest on, and
		 * the clearest demonstration that the tint is a real composite:
		 * the same panel colour reads violet at the top of the screen
		 * and amber at the bottom, because it actually is. */
		.name = "SUNSET",
		.bg_top = NEXUS_C(0x170B2Cu), .bg_bot = NEXUS_C(0xD95A32u),
		.glow_a = NEXUS_C(0xFFB450u), .glow_b = NEXUS_C(0xFF783Cu),
		.glow_alpha = 16,
		.panel = NEXUS_C(0x140622u), .panel_alpha = 88,
		.edge_hi = NEXUS_C(0xFFEBD7u), .edge_hi_alpha = 78,
		.edge_lo = NEXUS_C(0x14040Eu), .edge_lo_alpha = 90,
		.border = NEXUS_C(0xFFD6B4u), .border_alpha = 66,
		.caption = NEXUS_C(0xE0B3C8u), .value = NEXUS_C(0xFFF6EEu),
		.accent = NEXUS_C(0xFFD166u), .accent_alt = NEXUS_C(0xFFF0C4u),
		.muted = NEXUS_C(0xA87A92u), .track = NEXUS_C(0x1A0A18u),
		.success = NEXUS_C(0x8FE0A0u), .warning = NEXUS_C(0xFFD166u),
		.error = NEXUS_C(0xFF6B8Bu),
		.wordmark = { NEXUS_C(0xFFF6DCu), NEXUS_C(0xFFE9A8u),
			      NEXUS_C(0xFFB347u), NEXUS_C(0xC4506Bu),
			      NEXUS_C(0x5A1430u) },
		.radius = 7,
	},
};

static const struct nexus_theme *g_active;

const struct nexus_theme *nexus_theme(void)
{
	if (g_active == NULL) {
		/* First call decides. An unknown CONFIG name falls back to [0]
		 * rather than failing a firmware build over a typo. */
		nexus_theme_set(CONFIG_NEXUS_THEME);
	}
	return g_active;
}

int nexus_theme_set(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(themes); i++) {
		if (name && strcmp(themes[i].name, name) == 0) {
			g_active = &themes[i];
			return 0;
		}
	}

	g_active = &themes[0];
	return -ENOENT;
}

uint8_t nexus_theme_index(void)
{
	const struct nexus_theme *cur = nexus_theme();

	for (size_t i = 0; i < ARRAY_SIZE(themes); i++) {
		if (&themes[i] == cur) {
			return (uint8_t)i;
		}
	}
	return 0;
}

int nexus_theme_set_index(uint8_t index)
{
	if (index >= ARRAY_SIZE(themes)) {
		return -ERANGE;
	}
	g_active = &themes[index];
	return 0;
}

void nexus_theme_cycle(int delta)
{
	int n = (int)ARRAY_SIZE(themes);
	int i = ((int)nexus_theme_index() + delta) % n;

	/* C's % keeps the sign of the dividend, so a backwards step off zero
	 * lands negative and would index out of the table. */
	nexus_theme_set_index((uint8_t)(i < 0 ? i + n : i));
}

uint8_t nexus_theme_count(void)
{
	return (uint8_t)ARRAY_SIZE(themes);
}

const char *nexus_theme_name_at(uint8_t index)
{
	return (index < ARRAY_SIZE(themes)) ? themes[index].name : NULL;
}
