/**
   @file dsme_wd.h

   This file has defines hardware watchdog kicker.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.

   @author Igor Stoppa <igor.stopaa@nokia.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DSME_WD_H
#define DSME_WD_H

#include <stdbool.h>

/* Period for kicking; i.e. how often dsme wakes up to kick the watchdogs */
#define DSME_WD_PERIOD 12 /* seconds */

#ifdef __cplusplus
extern "C" {
#endif

void dsme_wd_kick(void);
bool dsme_wd_init(void);

#ifdef __cplusplus
}
#endif

#endif
