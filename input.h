/*	$OpenBSD$ */
/*
 * Copyright (c) 2015 Martin Pieuchot <mpi@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Linux input compatibiliby defines. */

#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H

/* These are wscons(4) value +1. */
#define BTN_MOUSE		BTN_LEFT
#define BTN_LEFT		1			/* 0x110 */
#define BTN_MIDDLE		2			/* 0x112 */
#define BTN_RIGHT		3			/* 0x111 */

/* These are currently unsupported. */
#define BTN_SIDE		8			/* 0x113 */
#define BTN_EXTRA		9			/* 0x114 */
#define BTN_FORWARD		10			/* 0x115 */
#define BTN_BACK		11			/* 0x116 */
#define BTN_TASK		12			/* 0x117 */

#define BTN_JOYSTICK		21			/* 0x120 */

#define KEY_MAX			255
#define KEY_CNT			KEY_MAX+1

#endif /* LINUX_INPUT_H */
