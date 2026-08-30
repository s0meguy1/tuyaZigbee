/********************************************************************************************************
 * @file    moes_eui.c
 *
 * @brief   See moes_eui.h.
 *
 * @date    2026
 *******************************************************************************************************/
#include "moes_eui.h"

static bool moes_euiHexNibble(unsigned char character, unsigned char *nibble)
{
	if(character >= '0' && character <= '9'){
		*nibble = character - '0';
		return true;
	}
	if(character >= 'a' && character <= 'f'){
		*nibble = character - 'a' + 10;
		return true;
	}
	if(character >= 'A' && character <= 'F'){
		*nibble = character - 'A' + 10;
		return true;
	}
	return false;
}

bool moes_euiParseDisplayAscii(const unsigned char displayAscii[MOES_EUI_ASCII_LEN],
                               unsigned char sdkEui[MOES_EUI_BYTES])
{
	unsigned char displayEui[MOES_EUI_BYTES];
	unsigned char high;
	unsigned char low;
	unsigned char index;

	if(!displayAscii || !sdkEui){
		return false;
	}

	/* Do every fallible operation against a local display-order value first.
	 * That keeps the output untouched for malformed or foreign factory data. */
	for(index = 0; index < MOES_EUI_BYTES; index++){
		if(!moes_euiHexNibble(displayAscii[index * 2], &high) ||
		   !moes_euiHexNibble(displayAscii[index * 2 + 1], &low)){
			return false;
		}
		displayEui[index] = (unsigned char)((high << 4) | low);
	}

	if(displayEui[0] != 0xa4 || displayEui[1] != 0xc1 || displayEui[2] != 0x38){
		return false;
	}

	for(index = 0; index < MOES_EUI_BYTES; index++){
		sdkEui[index] = displayEui[MOES_EUI_BYTES - 1 - index];
	}
	return true;
}
