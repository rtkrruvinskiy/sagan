/*
** Copyright (C) 2009-2017 Quadrant Information Security <quadrantsec.com>
** Copyright (C) 2009-2017 Champ Clark III <cclark@quadrantsec.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
 * sagan-xbit.c - Functions used for tracking events over multiple log
 * lines.
 *
 */


#ifdef HAVE_CONFIG_H
#include "config.h"             /* From autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "sagan.h"
#include "sagan-defs.h"
#include "sagan-ipc.h"
#include "sagan-xbit.h"
#include "sagan-rules.h"
#include "sagan-config.h"
#include "parsers/parsers.h"

struct _SaganCounters *counters;
struct _Rule_Struct *rulestruct;
struct _SaganDebug *debug;
struct _SaganConfig *config;

pthread_mutex_t Xbit_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t CounterMutex;	/* From sagan-engine.c */

struct _Sagan_IPC_Counters *counters_ipc;
struct _Sagan_IPC_Xbit *xbit_ipc;

/*****************************************************************************
 * Sagan_Xbit_Condition - Used for testing "isset" & "isnotset".  Full
 * rule condition is tested here and returned.
 *****************************************************************************/

int Sagan_Xbit_Condition(int rule_position, char *ip_src_char, char *ip_dst_char )
{

    time_t t;
    struct tm *now;
    char  timet[20];
    char  tmp[128] = { 0 };
    char *tmp_xbit_name = NULL;
    char *tok = NULL;

    int i;
    int a;

    uint32_t ip_src;
    uint32_t ip_dst;

    sbool xbit_match = false;
    int xbit_total_match = 0;

    t = time(NULL);
    now=localtime(&t);
    strftime(timet, sizeof(timet), "%s",  now);

    ip_src = IP2Bit(ip_src_char);
    ip_dst = IP2Bit(ip_dst_char);

    int and_or = false;

    Sagan_Xbit_Cleanup();

    for (i = 0; i < rulestruct[rule_position].xbit_count; i++) {

        /*******************
         *      ISSET      *
         *******************/

        if ( rulestruct[rule_position].xbit_type[i] == 3 ) {

            for (a = 0; a < counters_ipc->xbit_count; a++) {

                strlcpy(tmp, rulestruct[rule_position].xbit_name[i], sizeof(tmp));

                if (Sagan_strstr(rulestruct[rule_position].xbit_name[i], "|")) {
                    tmp_xbit_name = strtok_r(tmp, "|", &tok);
                    and_or = true;
                } else {
                    tmp_xbit_name = strtok_r(tmp, "&", &tok);
                    and_or = false; 					/* Need this? */
                }

                while (tmp_xbit_name != NULL ) {

                    if (!strcmp(tmp_xbit_name, xbit_ipc[a].xbit_name) &&
                        xbit_ipc[a].xbit_state == true ) {

                        /* direction: none */

                        if ( rulestruct[rule_position].xbit_direction[i] == 0 )

                        {

                            if ( debug->debugxbit ) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"isset\" xbit \"%s\" (direction: \"none\"). (any -> any)", __FILE__, __LINE__, xbit_ipc[a].xbit_name);
                            }

                            xbit_total_match++;
                        }

                        /* direction: both */

                        if ( rulestruct[rule_position].xbit_direction[i] == 1 &&
                             xbit_ipc[a].ip_src == ip_src &&
                             xbit_ipc[a].ip_dst == ip_dst )

                        {

                            if ( debug->debugxbit ) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"isset\" xbit \"%s\" (direction: \"both\"). (%s -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_src_char, ip_dst_char);
                            }

                            xbit_total_match++;
                        }

                        /* direction: by_src */

                        if ( rulestruct[rule_position].xbit_direction[i] == 2 &&
                             xbit_ipc[a].ip_src == ip_src )

                        {

                            if ( debug->debugxbit ) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"isset\" xbit \"%s\" (direction: \"by_src\"). (%s -> any)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_src_char);
                            }

                            xbit_total_match++;
                        }

                        /* direction: by_dst */

                        if ( rulestruct[rule_position].xbit_direction[i] == 3 &&
                             xbit_ipc[a].ip_dst == ip_dst )

                        {

                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"isset\" xbit \"%s\" (direction: \"by_dst\"). (any -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_dst_char);
                            }

                            xbit_total_match++;
                        }

                        /* direction: reverse */

                        if ( rulestruct[rule_position].xbit_direction[i] == 4 &&
                             xbit_ipc[a].ip_src == ip_dst &&
                             xbit_ipc[a].ip_dst == ip_src )

                        {
                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"isset\" xbit \"%s\" (direction: \"reverse\"). (%s -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_dst_char, ip_src_char);
                            }

                            xbit_total_match++;
                        }

                    } /* End of strcmp xbit_name & xbit_state = true */


                    if ( and_or == true) {
                        tmp_xbit_name = strtok_r(NULL, "|", &tok);
                    } else {
                        tmp_xbit_name = strtok_r(NULL, "&", &tok);
                    }

                } /* End of "while tmp_xbit_name" */

            } /* End of "for a" */

        } /* End "if" xbit_type == 3 (ISSET) */

        /*******************
        *    ISNOTSET     *
        *******************/

        if ( rulestruct[rule_position].xbit_type[i] == 4 ) {

            xbit_match = false;

            for (a = 0; a < counters_ipc->xbit_count; a++) {

                strlcpy(tmp, rulestruct[rule_position].xbit_name[i], sizeof(tmp));

                if (Sagan_strstr(rulestruct[rule_position].xbit_name[i], "|")) {
                    tmp_xbit_name = strtok_r(tmp, "|", &tok);
                    and_or = true;
                } else {
                    tmp_xbit_name = strtok_r(tmp, "&", &tok);
                    and_or = false;                                  /* Need this? */
                }

                while (tmp_xbit_name != NULL ) {

                    if (!strcmp(tmp_xbit_name, xbit_ipc[a].xbit_name)) {

                        xbit_match = true;

                        if ( xbit_ipc[a].xbit_state == false ) {

                            /* direction: none */

                            if ( rulestruct[rule_position].xbit_direction[i] == 0 ) {

                                if ( debug->debugxbit) {
                                    Sagan_Log(S_DEBUG, "[%s, line %d] \"isnotset\" xbit \"%s\" (direction: \"none\"). (any -> any)", __FILE__, __LINE__, xbit_ipc[a].xbit_name);
                                }

                                xbit_total_match++;
                            }

                            /* direction: both */

                            if ( rulestruct[rule_position].xbit_direction[i] == 1 ) {


                                if ( xbit_ipc[a].ip_src == ip_src &&
                                     xbit_ipc[a].ip_dst == ip_dst ) {

                                    if ( debug->debugxbit) {
                                        Sagan_Log(S_DEBUG, "[%s, line %d] \"isnotset\" xbit \"%s\" (direction: \"both\"). (%s -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_src_char, ip_dst_char);
                                    }

                                    xbit_total_match++;
                                }
                            }

                            /* direction: by_src */

                            if ( rulestruct[rule_position].xbit_direction[i] == 2 ) {

                                if ( xbit_ipc[a].ip_src == ip_src ) {

                                    if ( debug->debugxbit) {
                                        Sagan_Log(S_DEBUG, "[%s, line %d] \"isnotset\" xbit \"%s\" (direction: \"by_src\"). (%s -> any)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_src_char);
                                    }

                                    xbit_total_match++;
                                }
                            }

                            /* direction: by_dst */

                            if ( rulestruct[rule_position].xbit_direction[i] == 3 ) {

                                if ( xbit_ipc[a].ip_dst == ip_dst ) {

                                    if ( debug->debugxbit) {
                                        Sagan_Log(S_DEBUG, "[%s, line %d] \"isnotset\" xbit \"%s\" (direction: \"by_dst\"). (any -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_dst_char);
                                    }

                                    xbit_total_match++;
                                }
                            }

                            /* direction: reverse */

                            if ( rulestruct[rule_position].xbit_direction[i] == 4 ) {

                                if ( xbit_ipc[a].ip_src == ip_dst &&
                                     xbit_ipc[a].ip_dst == ip_src ) {

                                    if ( debug->debugxbit) {
                                        Sagan_Log(S_DEBUG, "[%s, line %d] \"isnotset\" xbit \"%s\" (direction: \"reverse\"). (%s -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_dst_char, ip_src_char);
                                    }

                                    xbit_total_match++;
                                }
                            }

                        } /* End xbit_state == false */

                    } /* End of strcmp xbit_name */

                    if ( and_or == true ) {
                        tmp_xbit_name = strtok_r(NULL, "|", &tok);
                    } else {
                        tmp_xbit_name = strtok_r(NULL, "&", &tok);
                    }

                } /* End of "while tmp_xbit_name" */
            } /* End of "for a" */

            if ( and_or == true && xbit_match == true ) {
                xbit_total_match = rulestruct[rule_position].xbit_condition_count;	/* Do we even need this for OR? */
            }

            if ( and_or == false && xbit_match == false ) {
                xbit_total_match = rulestruct[rule_position].xbit_condition_count;
            }

        } /* End of "xbit_type[i] == 4" */

    } /* End of "for i" */

    /* IF we match all criteria for isset/isnotset
     *
     * If we match the xbit_conditon_count (number of concurrent xbits)
     * we trigger.  It it's an "or" statement,  we trigger if any of the
     * xbits are set.
     *
     */

    if ( ( rulestruct[rule_position].xbit_condition_count == xbit_total_match ) || ( and_or == true && xbit_total_match != 0 ) ) {

        if ( debug->debugxbit) {
            Sagan_Log(S_DEBUG, "[%s, line %d] Condition of xbit returning TRUE. %d %d", __FILE__, __LINE__, rulestruct[rule_position].xbit_condition_count, xbit_total_match);
        }

        return(true);
    }

    /* isset/isnotset failed. */

    if ( debug->debugxbit) {
        Sagan_Log(S_DEBUG, "[%s, line %d] Condition of xbit returning FALSE. %d %d", __FILE__, __LINE__, rulestruct[rule_position].xbit_condition_count, xbit_total_match);
    }

    return(false);

}  /* End of Sagan_Xbit_Condition(); */


/*****************************************************************************
 * Sagan_Xbit_Set - Used to "set" & "unset" xbit.  All rule "set" and
 * "unset" happen here.
 *****************************************************************************/

void Sagan_Xbit_Set(int rule_position, char *ip_src_char, char *ip_dst_char )
{

    int i = 0;
    int a = 0;

    time_t t;
    struct tm *now;
    char  timet[20];

    char tmp[128] = { 0 };
    char *tmp_xbit_name = NULL;
    char *tok = NULL;

    sbool xbit_match = false;
    sbool xbit_unset_match = 0;

    uint32_t ip_src = 0;
    uint32_t ip_dst = 0;

    ip_src = IP2Bit(ip_src_char);
    ip_dst = IP2Bit(ip_dst_char);

    t = time(NULL);
    now=localtime(&t);
    strftime(timet, sizeof(timet), "%s",  now);

    struct _Sagan_Xbit_Track *xbit_track;

    xbit_track = malloc(sizeof(_Sagan_Xbit_Track));

    if ( xbit_track  == NULL ) {
        Sagan_Log(S_ERROR, "[%s, line %d] Failed to allocate memory for xbit_track. Abort!", __FILE__, __LINE__);
    }

    memset(xbit_track, 0, sizeof(_Sagan_Xbit_Track));

    int xbit_track_count = 0;

    Sagan_Xbit_Cleanup();


    for (i = 0; i < rulestruct[rule_position].xbit_count; i++) {

        /*******************
         *      UNSET      *
         *******************/

        if ( rulestruct[rule_position].xbit_type[i] == 2 ) {

            /* Xbits & (ie - bit1&bit2) */

            strlcpy(tmp, rulestruct[rule_position].xbit_name[i], sizeof(tmp));
            tmp_xbit_name = strtok_r(tmp, "&", &tok);

            while( tmp_xbit_name != NULL ) {


                for (a = 0; a < counters_ipc->xbit_count; a++) {

                    if ( !strcmp(tmp_xbit_name, xbit_ipc[a].xbit_name )) {

                        /* direction: none */

                        if ( rulestruct[rule_position].xbit_direction[i] == 0 ) {

                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"unset\" xbit \"%s\" (direction: \"none\"). (any -> any)", __FILE__, __LINE__, xbit_ipc[a].xbit_name);
                            }


                            Sagan_File_Lock(config->shm_xbit);
                            pthread_mutex_lock(&Xbit_Mutex);

                            xbit_ipc[a].xbit_state = false;

                            pthread_mutex_unlock(&Xbit_Mutex);
                            Sagan_File_Unlock(config->shm_xbit);

                            xbit_unset_match = 1;

                        }


                        /* direction: both */

                        if ( rulestruct[rule_position].xbit_direction[i] == 1 &&
                             xbit_ipc[a].ip_src == ip_src &&
                             xbit_ipc[a].ip_dst == ip_dst ) {

                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"unset\" xbit \"%s\" (direction: \"both\"). (%s -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_src_char, ip_dst_char);
                            }

                            Sagan_File_Lock(config->shm_xbit);
                            pthread_mutex_lock(&Xbit_Mutex);

                            xbit_ipc[a].xbit_state = false;

                            pthread_mutex_unlock(&Xbit_Mutex);
                            Sagan_File_Unlock(config->shm_xbit);

                            xbit_unset_match = 1;

                        }

                        /* direction: by_src */

                        if ( rulestruct[rule_position].xbit_direction[i] == 2 &&
                             xbit_ipc[a].ip_src == ip_src )

                        {

                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"unset\" xbit \"%s\" (direction: \"by_src\"). (%s -> any)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_src_char);
                            }

                            Sagan_File_Lock(config->shm_xbit);
                            pthread_mutex_lock(&Xbit_Mutex);

                            xbit_ipc[a].xbit_state = false;

                            pthread_mutex_unlock(&Xbit_Mutex);
                            Sagan_File_Unlock(config->shm_xbit);

                            xbit_unset_match = 1;

                        }


                        /* direction: by_dst */

                        if ( rulestruct[rule_position].xbit_direction[i] == 3 &&
                             xbit_ipc[a].ip_dst == ip_dst ) {

                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"unset\" xbit \"%s\" (direction: \"by_dst\"). (any -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_dst_char);
                            }

                            Sagan_File_Lock(config->shm_xbit);
                            pthread_mutex_lock(&Xbit_Mutex);

                            xbit_ipc[a].xbit_state = false;

                            pthread_mutex_unlock(&Xbit_Mutex);
                            Sagan_File_Unlock(config->shm_xbit);

                            xbit_unset_match = 1;

                        }

                        /* direction: reverse */

                        if ( rulestruct[rule_position].xbit_direction[i] == 4 &&
                             xbit_ipc[a].ip_dst == ip_src &&
                             xbit_ipc[a].ip_src == ip_dst )

                        {

                            if ( debug->debugxbit) {
                                Sagan_Log(S_DEBUG, "[%s, line %d] \"unset\" xbit \"%s\" (direction: \"reverse\"). (%s -> %s)", __FILE__, __LINE__, xbit_ipc[a].xbit_name, ip_dst_char, ip_src_char);
                            }

                            Sagan_File_Lock(config->shm_xbit);
                            pthread_mutex_lock(&Xbit_Mutex);

                            xbit_ipc[a].xbit_state = false;

                            pthread_mutex_unlock(&Xbit_Mutex);
                            Sagan_File_Unlock(config->shm_xbit);

                            xbit_unset_match = 1;

                        }
                    }
                }

                if ( debug->debugxbit && xbit_unset_match == 0 ) {
                    Sagan_Log(S_DEBUG, "[%s, line %d] No xbit found to \"unset\" for %s.", __FILE__, __LINE__, tmp_xbit_name);
                }

                tmp_xbit_name = strtok_r(NULL, "&", &tok);
            }
        } /* While & xbits (ie - bit1&bit2) */

        /*******************
         *      SET        *
        *******************/

        if ( rulestruct[rule_position].xbit_type[i] == 1 ) {

            xbit_match = false;

            /* Xbits & (ie - bit1&bit2) */

            strlcpy(tmp, rulestruct[rule_position].xbit_name[i], sizeof(tmp));
            tmp_xbit_name = strtok_r(tmp, "&", &tok);

            while( tmp_xbit_name != NULL ) {

                for (a = 0; a < counters_ipc->xbit_count; a++) {

                    /* Do we have the xbit already in memory?  If so,  update the information */

                    if (!strcmp(xbit_ipc[a].xbit_name, tmp_xbit_name) &&
                        xbit_ipc[a].ip_src == ip_src &&
                        xbit_ipc[a].ip_dst == ip_dst ) {

                        Sagan_File_Lock(config->shm_xbit);
                        pthread_mutex_lock(&Xbit_Mutex);

                        xbit_ipc[a].xbit_date = atol(timet);
                        xbit_ipc[a].xbit_expire = atol(timet) + rulestruct[rule_position].xbit_timeout[i];
                        xbit_ipc[a].xbit_state = true;

                        if ( debug->debugxbit) {
                            Sagan_Log(S_DEBUG, "[%s, line %d] [%d] Updated via \"set\" for xbit \"%s\", [%d].  New expire time is %d (%d) [%u -> %u]. ", __FILE__, __LINE__, a, tmp_xbit_name, i, xbit_ipc[i].xbit_expire, rulestruct[rule_position].xbit_timeout[i], xbit_ipc[a].ip_src, xbit_ipc[a].ip_dst);
                        }

                        pthread_mutex_unlock(&Xbit_Mutex);
                        Sagan_File_Unlock(config->shm_xbit);

                        xbit_match = true;
                    }

                }


                /* If the xbit isn't in memory,  store it to be created later */

                if ( xbit_match == false ) {

                    xbit_track = ( _Sagan_Xbit_Track * ) realloc(xbit_track, (xbit_track_count+1) * sizeof(_Sagan_Xbit_Track));

                    if ( xbit_track == NULL ) {
                        Sagan_Log(S_ERROR, "[%s, line %d] Failed to reallocate memory for xbit_track. Abort!", __FILE__, __LINE__);
                    }

                    strlcpy(xbit_track[xbit_track_count].xbit_name, tmp_xbit_name, sizeof(xbit_track[xbit_track_count].xbit_name));
                    xbit_track[xbit_track_count].xbit_timeout = rulestruct[rule_position].xbit_timeout[i];
                    xbit_track_count++;

                }

                tmp_xbit_name = strtok_r(NULL, "&", &tok);

            } /* While & xbits (ie - bit1&bit2) */

        } /* if xbit_type == 1 */

    } /* Out of for i loop */

    /* Do we have any xbits in memory that need to be created?  */

    if ( xbit_track_count != 0 ) {

        for (i = 0; i < xbit_track_count; i++) {

            if ( Sagan_Clean_IPC_Object(XBIT) == 0 ) {

                Sagan_File_Lock(config->shm_xbit);
                pthread_mutex_lock(&Xbit_Mutex);

                xbit_ipc[counters_ipc->xbit_count].ip_src = ip_src;
                xbit_ipc[counters_ipc->xbit_count].ip_dst = ip_dst;
                xbit_ipc[counters_ipc->xbit_count].xbit_date = atol(timet);
                xbit_ipc[counters_ipc->xbit_count].xbit_expire = atol(timet) + xbit_track[i].xbit_timeout;
                xbit_ipc[counters_ipc->xbit_count].xbit_state = true;
                xbit_ipc[counters_ipc->xbit_count].expire = xbit_track[i].xbit_timeout;

                strlcpy(xbit_ipc[counters_ipc->xbit_count].xbit_name, xbit_track[i].xbit_name, sizeof(xbit_ipc[counters_ipc->xbit_count].xbit_name));

                pthread_mutex_unlock(&Xbit_Mutex);
                Sagan_File_Unlock(config->shm_xbit);

                if ( debug->debugxbit) {
                    Sagan_Log(S_DEBUG, "[%s, line %d] [%d] Created xbit \"%s\" via \"set\" [%s -> %s],", __FILE__, __LINE__, counters_ipc->xbit_count, xbit_ipc[counters_ipc->xbit_count].xbit_name, ip_src_char, ip_dst_char);
                }

                Sagan_File_Lock(config->shm_counters);
                pthread_mutex_lock(&CounterMutex);

                counters_ipc->xbit_count++;

                pthread_mutex_unlock(&CounterMutex);
                Sagan_File_Unlock(config->shm_counters);

            }
        }
    }

    free(xbit_track);

} /* End of Sagan_Xbit_Set */


/*****************************************************************************
 * Sagan_Xbit_Type - Defines the "type" of xbit tracking based on user
 * input
 *****************************************************************************/

int Sagan_Xbit_Type ( char *type, int linecount, const char *ruleset )
{

    if (!strcmp(type, "none")) {
        return(0);
    }

    if (!strcmp(type, "both")) {
        return(1);
    }

    if (!strcmp(type, "by_src")) {
        return(2);
    }

    if (!strcmp(type, "by_dst")) {
        return(3);
    }

    if (!strcmp(type, "reverse")) {
        return(4);
    }

    Sagan_Log(S_ERROR, "[%s, line %d] Expected 'none', 'both', by_src', 'by_dst' or 'reverse'.  Got '%s' at line %d.", __FILE__, __LINE__, type, linecount, ruleset);

    return(0); 	/* Should never make it here */

}


/*****************************************************************************
 * Sagan_Xbit_Cleanup - Find "expired" xbits and toggle the "state"
 * to "off"
 *****************************************************************************/

void Sagan_Xbit_Cleanup(void)
{

    int i = 0;

    time_t t;
    struct tm *now;
    char  timet[20];

    t = time(NULL);
    now=localtime(&t);
    strftime(timet, sizeof(timet), "%s",  now);


    for (i=0; i<counters_ipc->xbit_count; i++) {
        if (  xbit_ipc[i].xbit_state == true && atol(timet) >= xbit_ipc[i].xbit_expire ) {
            if (debug->debugxbit) {
                Sagan_Log(S_DEBUG, "[%s, line %d] Setting xbit %s to \"expired\" state.", __FILE__, __LINE__, xbit_ipc[i].xbit_name);
            }
            xbit_ipc[i].xbit_state = false;
        }
    }

}
