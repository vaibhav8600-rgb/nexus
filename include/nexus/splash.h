/*
 * Splash artwork contract (Sections 19-21).
 *
 * Exactly one translation unit defines `nexus_splash_art`: either
 * assets/splash_default.c (drawn with primitives) or the file the build
 * generates from the integrator's PNG (a blitted bitmap). src/ui/splash.c
 * never learns which one it got, so Section 21's priority - user asset first,
 * NEXUS default second - is settled at link time with no runtime branch.
 */
#ifndef NEXUS_SPLASH_H_
#define NEXUS_SPLASH_H_

#ifdef __cplusplus
extern "C" {
#endif

struct nexus_splash_art {
	int w;
	int h; /* 0 means "no artwork"; the splash then shows text only */
	/** Draw with the top-left corner at (@p x, @p y), in logical pixels. */
	void (*draw)(int x, int y);
};

extern const struct nexus_splash_art nexus_splash_art;

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_SPLASH_H_ */
