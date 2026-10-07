/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pulls the portable core into the driver build; the DDK's build utility only
 * compiles sources that sit in the driver's own directory. */
#include "../core/datafab.c"
#include "../core/scsi.c"
#include "../core/bot.c"