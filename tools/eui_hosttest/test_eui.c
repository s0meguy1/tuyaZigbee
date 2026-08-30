/* Executes the real light/moes_eui.c, not a host-side reimplementation. */
#include <stdio.h>
#include <string.h>

#include "../../light/moes_eui.h"

static int failures;

static void check(const char *what, int condition)
{
	if(!condition){
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

static void expect_valid(const char *name, const char ascii[MOES_EUI_ASCII_LEN],
					 const unsigned char expected[MOES_EUI_BYTES])
{
	unsigned char actual[MOES_EUI_BYTES] = {0};
	bool parsed = moes_euiParseDisplayAscii((const unsigned char *)ascii, actual);

	printf("  %-16s -> %s\n", name, parsed ? "parsed" : "rejected");
	check(name, parsed);
	check("valid EUI has exact SDK LSB-first bytes",
		  parsed && memcmp(actual, expected, sizeof(actual)) == 0);
}

static void expect_rejected_unchanged(const char *name, const char ascii[MOES_EUI_ASCII_LEN])
{
	unsigned char actual[MOES_EUI_BYTES] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAA, 0x55, 0x12, 0x34};
	unsigned char before[MOES_EUI_BYTES];
	bool parsed;

	memcpy(before, actual, sizeof(actual));
	parsed = moes_euiParseDisplayAscii((const unsigned char *)ascii, actual);
	printf("  %-16s -> %s\n", name, parsed ? "parsed (wrong)" : "rejected");
	check(name, !parsed);
	check("rejected EUI leaves output unchanged",
		  memcmp(actual, before, sizeof(actual)) == 0);
}

int main(void)
{
	/* Synthetic vector. The a4:c1:38 OUI is Telink's published prefix and must
	 * stay, because the parser validates it; the remaining five bytes are made
	 * up on purpose so no real device identity enters this tree. They are all
	 * distinct and carry hex letters, so the case tests below exercise real
	 * letter folding rather than digits only. */
	static const unsigned char expected[MOES_EUI_BYTES] =
		{0x4b, 0x3c, 0x2d, 0x1e, 0x0f, 0x38, 0xc1, 0xa4};
	static const char lower[MOES_EUI_ASCII_LEN] = "a4c1380f1e2d3c4b";
	static const char upper[MOES_EUI_ASCII_LEN] = "A4C1380F1E2D3C4B";
	static const char mixed[MOES_EUI_ASCII_LEN] = "a4C1380f1E2d3C4b";
	static const char badEarly[MOES_EUI_ASCII_LEN] = "g4c1380f1e2d3c4b";
	static const char badMiddle[MOES_EUI_ASCII_LEN] = "a4c1380f!e2d3c4b";
	static const char badLate[MOES_EUI_ASCII_LEN] = "a4c1380f1e2d3c4g";
	static const char wrongOui[MOES_EUI_ASCII_LEN] = "a4c1390f1e2d3c4b";

	printf("=== display-order factory ASCII -> SDK LSB-first EUI ===\n");
	expect_valid("lowercase sample", lower, expected);
	expect_valid("uppercase sample", upper, expected);
	expect_valid("mixed-case sample", mixed, expected);

	printf("=== failures are fail-closed ===\n");
	expect_rejected_unchanged("bad early digit", badEarly);
	expect_rejected_unchanged("bad middle digit", badMiddle);
	expect_rejected_unchanged("bad late digit", badLate);
	expect_rejected_unchanged("wrong Telink OUI", wrongOui);

	if(failures){
		printf("\n%d FAILING check(s)\n", failures);
		return 1;
	}
	printf("\nall EUI parsing checks passed (real moes_eui.c)\n");
	return 0;
}
