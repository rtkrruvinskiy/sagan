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

/* sagan-stats.c
 *
 * Simply dumps statistics of Sagan to the user or via sagan.log
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"             /* From autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include "sagan.h"
#include "sagan-defs.h"
#include "sagan-stats.h"
#include "sagan-config.h"

struct _SaganCounters *counters;
struct _Sagan_IPC_Counters *counters_ipc;

struct _SaganConfig *config;

void Sagan_Statistics( void )
{

    char timet[20];

    time_t t;
    struct tm *now;
    int seconds = 0;
    unsigned long total=0;

    int uptime_days;
    int uptime_abovedays;
    int uptime_hours;
    int uptime_abovehours;
    int uptime_minutes;
    int uptime_seconds;

#ifdef WITH_BLUEDOT
    unsigned long bluedot_ip_total=0;
    unsigned long bluedot_hash_total=0;
    unsigned long bluedot_url_total=0;
    unsigned long bluedot_filename_total=0;
#endif


    /* This is used to calulate the events per/second */
    /* Champ Clark III - 11/17/2011 */

    t = time(NULL);
    now=localtime(&t);
    strftime(timet, sizeof(timet), "%s",  now);
    seconds = atol(timet) - atol(config->sagan_startutime);

    /* if statement prevents floating point exception */

    if ( seconds != 0 ) {
        total = counters->sagantotal / seconds;

#ifdef WITH_BLUEDOT
        bluedot_ip_total = counters->bluedot_ip_total / seconds;
        bluedot_hash_total = counters->bluedot_hash_total / seconds;
        bluedot_url_total = counters->bluedot_url_total / seconds;
        bluedot_filename_total = counters->bluedot_filename_total / seconds;
#endif

    }


    if ((isatty(1))) {


        Sagan_Log(S_NORMAL, " ,-._,-.  -[ Sagan Version %s - Engine Statistics ]-", VERSION);
        Sagan_Log(S_NORMAL, " \\/)\"(\\/");
        Sagan_Log(S_NORMAL, "  (_o_)    Events processed         : %" PRIuMAX "", counters->sagantotal);
        Sagan_Log(S_NORMAL, "  /   \\/)  Signatures matched       : %" PRIuMAX " (%.3f%%)", counters->saganfound, CalcPct(counters->saganfound, counters->sagantotal ) );
        Sagan_Log(S_NORMAL, " (|| ||)   Alerts                   : %" PRIuMAX " (%.3f%%)",  counters->alert_total, CalcPct( counters->alert_total, counters->sagantotal) );
        Sagan_Log(S_NORMAL, "  oo-oo    After                    : %" PRIuMAX " (%.3f%%)",  counters->after_total, CalcPct( counters->after_total, counters->sagantotal) );
        Sagan_Log(S_NORMAL, "           Threshold                : %" PRIuMAX " (%.3f%%)", counters->threshold_total, CalcPct( counters->threshold_total, counters->sagantotal) );
        Sagan_Log(S_NORMAL, "           Dropped                  : %" PRIuMAX " (%.3f%%)", counters->sagan_processor_drop + counters->sagan_output_drop + counters->sagan_log_drop, CalcPct(counters->sagan_processor_drop + counters->sagan_output_drop + counters->sagan_log_drop, counters->sagantotal) );

//        Sagan_Log(S_NORMAL, "           Malformed                : h:%" PRIuMAX "|f:%" PRIuMAX "|p:%" PRIuMAX "|l:%" PRIuMAX "|T:%" PRIuMAX "|d:%" PRIuMAX "|T:%" PRIuMAX "|P:%" PRIuMAX "|M:%" PRIuMAX "", counters->malformed_host, counters->malformed_facility, counters->malformed_priority, counters->malformed_level, counters->malformed_tag, counters->malformed_date, counters->malformed_time, counters->malformed_program, counters->malformed_message);

        Sagan_Log(S_NORMAL, "           Thread Exhaustion        : %" PRIuMAX " (%.3f%%)", counters->worker_thread_exhaustion,  CalcPct( counters->worker_thread_exhaustion, counters->sagantotal) );


        if (config->sagan_droplist_flag) {
            Sagan_Log(S_NORMAL, "           Ignored Input            : %" PRIuMAX " (%.3f%%)", counters->ignore_count, CalcPct(counters->ignore_count, counters->sagantotal) );
        }

#ifdef HAVE_LIBMAXMINDDB
        Sagan_Log(S_NORMAL, "           GeoIP2 Hits:             : %" PRIuMAX " (%.3f%%)", counters->geoip2_hit, CalcPct( counters->geoip2_hit, counters->sagantotal) );
        Sagan_Log(S_NORMAL, "           GeoIP2 Lookups:          : %" PRIuMAX "", counters->geoip2_lookup);
        Sagan_Log(S_NORMAL, "           GeoIP2 Misses            : %" PRIuMAX "", counters->geoip2_miss);
#endif

        uptime_days = seconds / 86400;
        uptime_abovedays = seconds % 86400;
        uptime_hours = uptime_abovedays / 3600;
        uptime_abovehours = uptime_abovedays % 3600;
        uptime_minutes = uptime_abovehours / 60;
        uptime_seconds = uptime_abovehours % 60;

        Sagan_Log(S_NORMAL, "           Uptime                   : %d days, %d hours, %d minutes, %d seconds.", uptime_days, uptime_hours, uptime_minutes, uptime_seconds);

        /* If processing from a file,  don't display events per/second */

        if ( config->sagan_is_file == 0 ) {

            if ( seconds < 60 || seconds == 0 ) {
                Sagan_Log(S_NORMAL, "           Avg. events per/second   : %lu [%lu of 60 seconds. Calculating...]", total, seconds);
            } else {
                Sagan_Log(S_NORMAL, "           Avg. events per/second   : %lu", total);
            }
        } else {

            Sagan_Log(S_NORMAL, "           Avg. events per/second   : %lu", total);

        }


        Sagan_Log(S_NORMAL, "");
        Sagan_Log(S_NORMAL, "          -[ Sagan Processor Statistics ]-");
        Sagan_Log(S_NORMAL, "");
        Sagan_Log(S_NORMAL, "           Dropped                  : %" PRIuMAX " (%.3f%%)", counters->sagan_processor_drop, CalcPct(counters->sagan_processor_drop, counters->sagantotal) );

        if (config->blacklist_flag) {
            Sagan_Log(S_NORMAL, "           Blacklist Lookups        : %" PRIuMAX " (%.3f%%)", counters->blacklist_lookup_count, CalcPct(counters->blacklist_lookup_count, counters->sagantotal) );
            Sagan_Log(S_NORMAL, "           Blacklist Hits           : %" PRIuMAX " (%.3f%%)", counters->blacklist_hit_count, CalcPct(counters->blacklist_hit_count, counters->sagantotal) );

        }

        if (config->sagan_track_clients_flag) {
            Sagan_Log(S_NORMAL, "           Tracking/Down            : %" PRIuMAX " / %"PRIuMAX " [%d minutes]" , counters_ipc->track_clients_client_count, counters_ipc->track_clients_down, config->pp_sagan_track_clients);
        }


        if (config->output_thread_flag) {
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          -[ Sagan Output Plugin Statistics ]-");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL,"           Dropped                  : %" PRIuMAX " (%.3f%%)", counters->sagan_output_drop, CalcPct(counters->sagan_output_drop, counters->sagantotal) );
        }

#ifdef HAVE_LIBESMTP
        if ( config->sagan_esmtp_flag ) {
            Sagan_Log(S_NORMAL, "           Email Success/Failed     : %" PRIuMAX " / %" PRIuMAX "" , counters->esmtp_count_success, counters->esmtp_count_failed);
        }
#endif


        if (config->syslog_src_lookup) {
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          -[ Sagan DNS Cache Statistics ]-");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "           Cached                   : %" PRIuMAX "", counters->dns_cache_count);
            Sagan_Log(S_NORMAL, "           Missed                   : %" PRIuMAX " (%.3f%%)", counters->dns_miss_count, CalcPct(counters->dns_miss_count, counters->dns_cache_count));
        }

        Sagan_Log(S_NORMAL, "");
        Sagan_Log(S_NORMAL, "          -[ Sagan follow_flow Statistics ]-");
        Sagan_Log(S_NORMAL, "");
        Sagan_Log(S_NORMAL, "           Total                    : %" PRIuMAX "", counters->follow_flow_total);
        Sagan_Log(S_NORMAL, "           Dropped                  : %" PRIuMAX " (%.3f%%)", counters->follow_flow_drop, CalcPct(counters->follow_flow_drop, counters->follow_flow_total));

#ifdef WITH_BLUEDOT

        if (config->bluedot_flag) {
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          -[ Sagan Bluedot Processor ]-");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          * IP Reputation *");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          IP addresses in cache         : %" PRIuMAX " (%.3f%%)", counters->bluedot_ip_cache_count, CalcPct(counters->bluedot_ip_cache_count, config->bluedot_max_cache));
            Sagan_Log(S_NORMAL, "          IP hits from cache            : %" PRIuMAX " (%.3f%%)", counters->bluedot_ip_cache_hit, CalcPct(counters->bluedot_ip_cache_hit, counters->bluedot_ip_cache_count));
            Sagan_Log(S_NORMAL, "          IP/Bluedot hits in logs       : %" PRIuMAX "", counters->bluedot_ip_positive_hit);
            Sagan_Log(S_NORMAL, "          IP with date > mdate          : %" PRIuMAX "", counters->bluedot_mdate);
            Sagan_Log(S_NORMAL, "          IP with date > cdate          : %" PRIuMAX "", counters->bluedot_cdate);
            Sagan_Log(S_NORMAL, "          IP with date > mdate [cache]  : %" PRIuMAX "", counters->bluedot_mdate_cache);
            Sagan_Log(S_NORMAL, "          IP with date > cdate [cache]  : %" PRIuMAX "", counters->bluedot_cdate_cache);
            Sagan_Log(S_NORMAL, "          IP queries per/second         : %lu", bluedot_ip_total);

            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          * File Hash *");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          Hashes in cache               : %" PRIuMAX " (%.3f%%)", counters->bluedot_hash_cache_count, CalcPct(counters->bluedot_hash_cache_count, config->bluedot_max_cache));
            Sagan_Log(S_NORMAL, "          Hash hits from cache          : %" PRIuMAX " (%.3f%%)", counters->bluedot_hash_cache_hit, CalcPct(counters->bluedot_hash_cache_hit, counters->bluedot_hash_cache_count));
            Sagan_Log(S_NORMAL, "          Hash/Bluedot hits in logs     : %" PRIuMAX "", counters->bluedot_hash_positive_hit);
            Sagan_Log(S_NORMAL, "          Hash queries per/second       : %lu", bluedot_hash_total);

            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          * URL Reputation *");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          URLs in cache                 : %" PRIuMAX " (%.3f%%)", counters->bluedot_url_cache_count, CalcPct(counters->bluedot_url_cache_count, config->bluedot_max_cache));
            Sagan_Log(S_NORMAL, "          URL hits from cache           : %" PRIuMAX " (%.3f%%)", counters->bluedot_url_cache_hit, CalcPct(counters->bluedot_url_cache_hit, counters->bluedot_url_cache_count));
            Sagan_Log(S_NORMAL, "          URL/Bluedot hits in logs      : %" PRIuMAX "", counters->bluedot_url_positive_hit);
            Sagan_Log(S_NORMAL, "          URL queries per/second        : %lu", bluedot_url_total);

            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          * Filename Reputation *");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          Filenames in cache            : %" PRIuMAX " (%.3f%%)", counters->bluedot_filename_cache_count, CalcPct(counters->bluedot_filename_cache_count, config->bluedot_max_cache));
            Sagan_Log(S_NORMAL, "          Filename hits from cache      : %" PRIuMAX " (%.3f%%)", counters->bluedot_filename_cache_hit, CalcPct(counters->bluedot_filename_cache_hit, counters->bluedot_filename_cache_count));
            Sagan_Log(S_NORMAL, "          Filename/Bluedot hits in logs : %" PRIuMAX "", counters->bluedot_filename_positive_hit);
            Sagan_Log(S_NORMAL, "          URL queries per/second        : %lu", bluedot_filename_total);

            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          * Bluedot Combined Statistics *");
            Sagan_Log(S_NORMAL, "");
            Sagan_Log(S_NORMAL, "          Lookup error count            : %" PRIuMAX "", counters->bluedot_error_count);
            Sagan_Log(S_NORMAL, "          Total query rate/per second   : %lu", bluedot_ip_total + bluedot_hash_total + bluedot_url_total + bluedot_filename_total);


        }
#endif


        Sagan_Log(S_NORMAL, "-------------------------------------------------------------------------------");


    }
}
