/*
 * wos_pad.c -- the PSX controller ports for the WarpOS build.
 *
 * The serial protocol (commands 0x40-0x4F: digital, analog and DualShock
 * config mode) is SSSPSX's, as Gamecube/PadSSSPSX.c has it; only where the
 * buttons come from differs: wos_input.c (keyboard and lowlevel.library
 * joyports) instead of the Wii's controller layer. No multitap, lightgun or
 * rumble.
 *
 *	PADwin
 *	Copyright (C) 2002-2004  PADwin Team
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 */
#include <stdint.h>
#include <string.h>

#include "../psxcommon.h"
#include "../plugins.h"
#include "../psemu_plugin_defs.h"
#include "../Gamecube/wiiSXconfig.h"
#include "wos_input.h"

PadDataS lastport1;
PadDataS lastport2;
long     PadFlags = 0;

static int pad_initialized = 0;

static struct
{
	u16 padStat[2];		/* digital buttons, as the poll sends them */
	int padID[2];
	int padMode1[2];	/* 0 = digital, 1 = analog */
	int padMode2[2];
	int padModeE[2];	/* config (escape) mode */
	int padModeC[2];
	int padModeF[2];
	int padVib0[2];
	int padVib1[2];
	int padVibF[2][4];
	int curPad;
	int curByte;
	int curCmd;
	int cmdLen;
	int irq10En[2];
	int isConnected[2];
} global;

long PAD__init(long flags)
{
	PadFlags |= flags;
	return PSE_PAD_ERR_SUCCESS;
}

long PAD__shutdown(void)
{
	return PSE_PAD_ERR_SUCCESS;
}

static void PADsetMode (const int pad, const int mode)	/* mode = 0 (digital) or 1 (analog) */
{
	static const u8 padID[] = { 0x41, 0x73, 0x41, 0x79 };
	global.padMode1[pad] = mode;
	global.padVib0[pad] = 0;
	global.padVib1[pad] = 0;
	global.padVibF[pad][0] = 0;
	global.padVibF[pad][1] = 0;
	global.padID[pad] = padID[global.padMode2[pad] * 2 + mode];
}

static void UpdateState (const int pad)
{
	WosPadState st;
	PadDataS *port = pad ? &lastport2 : &lastport1;

	if (global.padMode1[pad] != controllerType)
		PADsetMode(pad, controllerType == CONTROLLERTYPE_ANALOG ? 1 : 0);

	wos_input_read_pad(pad, &st);
	global.isConnected[pad] = st.connected;

	/* buttons: bit 0..7 the first data byte, 8..15 the second, active low;
	 * the poll sends padStat high byte first. */
	global.padStat[pad] = (u16)(((st.buttons & 0xFF) << 8) | (st.buttons >> 8));

	port->leftJoyX  = st.lx; port->leftJoyY  = st.ly;
	port->rightJoyX = st.rx; port->rightJoyY = st.ry;
	port->buttonStatus = global.padStat[pad];
}

long SSS_PADopen (void *p)
{
	int i;
	(void)p;
	if (!pad_initialized)
	{
		memset (&global, 0, sizeof (global));
		memset (&lastport1, 0, sizeof(lastport1));
		memset (&lastport2, 0, sizeof(lastport2));
		for (i = 0; i < 2; i++) {
			global.padStat[i] = 0xffff;
			PADsetMode (i, controllerType == CONTROLLERTYPE_ANALOG ? 1 : 0);
		}
		pad_initialized = 1;
	}
	return 0;
}

long SSS_PADclose (void)
{
	pad_initialized = 0;
	return 0;
}

unsigned char SSS_PADstartPoll (int pad)
{
	global.curPad = (pad - 1) & 1;
	global.curByte = 0;
	return 0xff;
}

void SSS_SetMultiPad(int pad, int mpad)
{
	(void)pad; (void)mpad;
}

static const u8 cmd40[8] =
{
	0xff, 0x5a, 0x00, 0x00, 0x02, 0x00, 0x00, 0x5a
};
static const u8 cmd41[8] = /* which buttons are in the poll response */
{
	0xff, 0x5a, 0xff, 0xff, 0x03, 0x00, 0x00, 0x5a,
};
static const u8 cmd44[8] = /* switch digital/analog */
{
	0xff, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const u8 cmd45[8] = /* status: DualShock, LED on */
{
	0xff, 0x5a, 0x03, 0x02, 0x01, 0x02, 0x01, 0x00,
};
static const u8 cmd46[8] =
{
	0xff, 0x5a, 0x00, 0x00, 0x01, 0x02, 0x00, 0x0a,
};
static const u8 cmd47[8] =
{
	0xff, 0x5a, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00,
};
static const u8 cmd4c[8] =
{
	0xff, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const u8 cmd4d[8] = /* map motors (config mode only) */
{
	0xff, 0x5a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const u8 cmd4f[8] =
{
	0xff, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a,
};

unsigned char SSS_PADpoll (const unsigned char value)
{
	const int pad = global.curPad;
	const int cur = global.curByte;
	PadDataS *port = pad ? &lastport2 : &lastport1;

	static union
	{
		u16 b16[20];
		u8  b8[40];
	} buf;

	if (cur == 0)
	{
		global.curByte++;
		global.curCmd = value;
		if (controllerType != CONTROLLERTYPE_ANALOG && value != 0x42)
			global.curCmd = 0x42;
		switch (global.curCmd)
		{
		case 0x40:
			global.cmdLen = sizeof (cmd40);
			memcpy (buf.b8, cmd40, sizeof (cmd40));
			return 0xf3;
		case 0x41:
			global.cmdLen = sizeof (cmd41);
			memcpy (buf.b8, cmd41, sizeof (cmd41));
			return 0xf3;
		case 0x42:
			UpdateState(pad);
			if (!global.isConnected[pad]) {
				global.cmdLen = 0;
				return 0xff;
			}
			/* fall through */
		case 0x43:
			global.cmdLen = 2 + 2 * (global.padID[pad] & 0x0f);
			buf.b8[1] = global.padModeC[pad] ? 0x00 : 0x5a;
			buf.b16[1] = global.padStat[pad];
			if (value == 0x43 && global.padModeE[pad])
			{
				buf.b16[2] = 0;
				buf.b16[3] = 0;
				return 0xf3;
			}
			buf.b8[4] = port->rightJoyX;
			buf.b8[5] = port->rightJoyY;
			buf.b8[6] = port->leftJoyX;
			buf.b8[7] = port->leftJoyY;
			return (u8)global.padID[pad];
		case 0x44:
			global.cmdLen = sizeof (cmd44);
			memcpy (buf.b8, cmd44, sizeof (cmd44));
			return 0xf3;
		case 0x45:
			global.cmdLen = sizeof (cmd45);
			memcpy (buf.b8, cmd45, sizeof (cmd45));
			buf.b8[4] = (u8)global.padMode1[pad];
			return 0xf3;
		case 0x46:
			global.cmdLen = sizeof (cmd46);
			memcpy (buf.b8, cmd46, sizeof (cmd46));
			return 0xf3;
		case 0x47:
			global.cmdLen = sizeof (cmd47);
			memcpy (buf.b8, cmd47, sizeof (cmd47));
			return 0xf3;
		case 0x4c:
			global.cmdLen = sizeof (cmd4c);
			memcpy (buf.b8, cmd4c, sizeof (cmd4c));
			return 0xf3;
		case 0x4d:
			global.cmdLen = sizeof (cmd4d);
			memcpy (buf.b8, cmd4d, sizeof (cmd4d));
			return 0xf3;
		case 0x4f:
			global.padID[pad] = 0x79;
			global.padMode2[pad] = 1;
			global.cmdLen = sizeof (cmd4f);
			memcpy (buf.b8, cmd4f, sizeof (cmd4f));
			return 0xf3;
		}
	}
	switch (global.curCmd)
	{
	case 0x42:
		if (cur == global.padVib0[pad])
			global.padVibF[pad][0] = value;
		if (cur == global.padVib1[pad])
			global.padVibF[pad][1] = value;
		if (cur == 2)
			global.irq10En[pad] = value;
		break;
	case 0x43:
		if (cur == 2)
		{
			global.padModeE[pad] = value;
			global.padModeC[pad] = 0;
		}
		break;
	case 0x44:
		if (cur == 2)
			PADsetMode (pad, value);
		if (cur == 3)
			global.padModeF[pad] = (value == 3);
		break;
	case 0x46:
		if (cur == 2)
		{
			switch (value)
			{
			case 0x00:
				buf.b8[5] = 0x02;
				buf.b8[6] = 0x00;
				buf.b8[7] = 0x0A;
				break;
			case 0x01:
				buf.b8[5] = 0x01;
				buf.b8[6] = 0x01;
				buf.b8[7] = 0x14;
				break;
			}
		}
		break;
	case 0x4c:
		if (cur == 2)
		{
			static const u8 buf5[] = { 0x04, 0x07, 0x02, 0x05 };
			buf.b8[5] = buf5[value & 0x03];
		}
		break;
	case 0x4d:
		if (cur >= 2)
		{
			if (cur == global.padVib0[pad])
				buf.b8[cur] = 0x00;
			else if (cur == global.padVib1[pad])
				buf.b8[cur] = 0x01;

			switch (value)
			{
			case 0x00:
				global.padVib0[pad] = cur;
				/* fall through */
			case 0x01:
				if ((global.padID[pad] & 0x0f) < (cur - 1) / 2)
					 global.padID[pad] = (global.padID[pad] & 0xf0) + (cur - 1) / 2;
				if (value) global.padVib1[pad] = cur;
			}
		}
		break;
	}

	if (cur >= global.cmdLen)
		return 0;
	return buf.b8[global.curByte++];
}

static long read_port(int pad, PadDataS *pads)
{
	UpdateState(pad);
	*pads = pad ? lastport2 : lastport1;
	pads->controllerType = global.padMode1[pad] ? PSE_PAD_TYPE_ANALOGPAD
	                                            : PSE_PAD_TYPE_STANDARD;
	return PSE_PAD_ERR_SUCCESS;
}

long SSS_PADreadPort1 (PadDataS* pads) { return read_port(0, pads); }
long SSS_PADreadPort2 (PadDataS* pads) { return read_port(1, pads); }
