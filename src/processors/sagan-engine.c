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

/* sagan-engine.c
 *
 * Threaded negine that looks for events & patterns based on 'Snort like'
 * rules.
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
#include <sys/stat.h>
#include <sys/mman.h>

#include "sagan.h"
#include "sagan-aetas.h"
#include "sagan-meta-content.h"
#include "sagan-send-alert.h"
#include "sagan-xbit.h"
#include "sagan-rules.h"
#include "sagan-config.h"
#include "sagan-ipc.h"
#include "sagan-check-flow.h"

#include "parsers/parsers.h"

#include "processors/sagan-engine.h"
#include "processors/sagan-bro-intel.h"
#include "processors/sagan-blacklist.h"
#include "processors/sagan-dynamic-rules.h"

#ifdef WITH_BLUEDOT
#include "processors/sagan-bluedot.h"
#endif

#ifdef HAVE_LIBLOGNORM
#include "sagan-liblognorm.h"
struct _SaganNormalizeLiblognorm *SaganNormalizeLiblognorm;
pthread_mutex_t Lognorm_Mutex;
#endif

#ifdef HAVE_LIBMAXMINDDB
#include <sagan-geoip2.h>
#endif

struct _SaganCounters *counters;
struct _Rule_Struct *rulestruct;
struct _SaganDebug *debug;
struct _SaganConfig *config;

struct _Sagan_IPC_Counters *counters_ipc;

pthread_mutex_t CounterMutex=PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t After_By_Src_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t After_By_Dst_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t After_By_Src_Port_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t After_By_Dst_Port_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t After_By_Username_Mutex=PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t Thresh_By_Src_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Thresh_By_Dst_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Thresh_By_Src_Port_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Thresh_By_Dst_Port_Mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Thresh_By_Username_Mutex=PTHREAD_MUTEX_INITIALIZER;

struct thresh_by_src_ipc *threshbysrc_ipc;
struct thresh_by_dst_ipc *threshbydst_ipc;
struct thresh_by_srcport_ipc *threshbysrcport_ipc;
struct thresh_by_dstport_ipc *threshbydstport_ipc;
struct thresh_by_username_ipc *threshbyusername_ipc;

struct after_by_src_ipc *afterbysrc_ipc;
struct after_by_dst_ipc *afterbydst_ipc;
struct after_by_srcport_ipc *afterbysrcport_ipc;
struct after_by_dstport_ipc *afterbydstport_ipc;
struct after_by_username_ipc *afterbyusername_ipc;

void Sagan_Engine_Init ( void )
{

#ifdef HAVE_LIBLOGNORM

    SaganNormalizeLiblognorm = malloc(sizeof(struct _SaganNormalizeLiblognorm));

    if ( SaganNormalizeLiblognorm == NULL ) {
        Sagan_Log(S_ERROR, "[%s, line %d] Failed to allocate memory for SaganNormalizeLiblognorm. Abort!", __FILE__, __LINE__);
    }

    memset(SaganNormalizeLiblognorm, 0, sizeof(_SaganNormalizeLiblognorm));

#endif

}

int Sagan_Engine ( _Sagan_Proc_Syslog *SaganProcSyslog_LOCAL, sbool dynamic_rule_flag )
{

    struct _Sagan_Processor_Info *processor_info_engine = NULL;
    processor_info_engine = malloc(sizeof(struct _Sagan_Processor_Info));

    if ( processor_info_engine == NULL ) {
        Sagan_Log(S_ERROR, "[%s, line %d] Failed to allocate memory for processor_info_engine. Abort!", __FILE__, __LINE__);
    }

    memset(processor_info_engine, 0, sizeof(_Sagan_Processor_Info));

    int processor_info_engine_src_port = 0;
    int processor_info_engine_dst_port = 0;
    int processor_info_engine_proto = 0;
    int processor_info_engine_alertid = 0;

    sbool after_log_flag = false;
    sbool after_flag = false;

    int threadid = 0;

    int i = 0;
    int b = 0;
    int z = 0;

    sbool match = false;
    int sagan_match = 0;				/* Used to determine if all has "matched" (content, pcre, meta_content, etc) */

    int rc = 0;
    int ovector[PCRE_OVECCOUNT];

    int alter_num = 0;
    int meta_alter_num = 0;

    sbool xbit_return = 0;
    sbool alert_time_trigger = false;
    sbool check_flow_return = true;  /* 1 = match, 0 = no match */

    char *ptmp;
    char *tok2;

    /* We don't tie these to HAVE_LIBMAXMINDDB because we might have other
     * methods to extract the informaton */

    char normalize_username[MAX_USERNAME_SIZE] = { 0 };
    char normalize_md5_hash[MD5_HASH_SIZE+1] = { 0 };
    char normalize_sha1_hash[SHA1_HASH_SIZE+1] = { 0 };
    char normalize_sha256_hash[SHA256_HASH_SIZE+1] = { 0 };

    char normalize_filename[MAX_FILENAME_SIZE] = { 0 };
    char normalize_http_uri[MAX_URL_SIZE] = { 0 };
    char normalize_http_hostname[MAX_HOSTNAME_SIZE] = { 0 };

    int  normalize_src_port;
    int  normalize_dst_port;

    char ip_src[MAXIP];
    sbool ip_src_flag = 0;

    uint32_t ip_src_u32;
    uint32_t ip_srcport_u32;

    char ip_dst[MAXIP];
    sbool ip_dst_flag = 0;

    uint32_t ip_dst_u32 = 0;
    uint32_t ip_dstport_u32 = 0;

    char tmpbuf[128];
    char s_msg[1024];
    char alter_content[MAX_SYSLOGMSG];
    char meta_alter_content[MAX_SYSLOGMSG];

    time_t t;
    struct tm *now;
    char  timet[20];

    uintmax_t thresh_oldtime;
    uintmax_t after_oldtime;

    sbool thresh_flag = false;
    sbool thresh_log_flag = false;

    int proto = config->sagan_proto;		/* Set proto to default */

    sbool brointel_results = 0;
    sbool blacklist_results = 0;

#ifdef HAVE_LIBMAXMINDDB

    unsigned char geoip2_return = 0;
    sbool geoip2_isset = false;

#endif

#ifdef WITH_BLUEDOT

    unsigned char bluedot_results = 0;
    sbool bluedot_ip_flag = 0;
    sbool bluedot_hash_flag = 0;
    sbool bluedot_url_flag = 0;
    sbool bluedot_filename_flag = 0;

#endif


    /* This needs to be included,  even if liblognorm isn't in use */

    sbool liblognorm_status = 0;

    /* Search for matches */

    /* First we search for 'program' and such.   This way,  we don't waste CPU
     * time with pcre/content.  */

    for(b=0; b < counters->rulecount; b++) {

        /* Process "normal" rules.  Skip dynamic rules if it's not time to process them */

        if ( rulestruct[b].type == NORMAL_RULE || ( rulestruct[b].type == DYNAMIC_RULE && dynamic_rule_flag == true ) ) {

            match = false;

            if ( strcmp(rulestruct[b].s_program, "" )) {
                strlcpy(tmpbuf, rulestruct[b].s_program, sizeof(tmpbuf));
                ptmp = strtok_r(tmpbuf, "|", &tok2);
                match = true;
                while ( ptmp != NULL ) {
                    if ( Sagan_Wildcard(ptmp, SaganProcSyslog_LOCAL->syslog_program) == 1 ) {
                        match = false;
                    }

                    ptmp = strtok_r(NULL, "|", &tok2);
                }
            }

            if ( strcmp(rulestruct[b].s_facility, "" )) {
                strlcpy(tmpbuf, rulestruct[b].s_facility, sizeof(tmpbuf));
                ptmp = strtok_r(tmpbuf, "|", &tok2);
                match = true;
                while ( ptmp != NULL ) {
                    if (!strcmp(ptmp, SaganProcSyslog_LOCAL->syslog_facility)) {
                        match = false;
                    }

                    ptmp = strtok_r(NULL, "|", &tok2);
                }
            }

            if ( strcmp(rulestruct[b].s_syspri, "" )) {
                strlcpy(tmpbuf, rulestruct[b].s_syspri, sizeof(tmpbuf));
                ptmp = strtok_r(tmpbuf, "|", &tok2);
                match = true;
                while ( ptmp != NULL ) {
                    if (!strcmp(ptmp, SaganProcSyslog_LOCAL->syslog_priority)) {
                        match = false;
                    }

                    ptmp = strtok_r(NULL, "|", &tok2);
                }
            }

            if ( strcmp(rulestruct[b].s_level, "" )) {
                strlcpy(tmpbuf, rulestruct[b].s_level, sizeof(tmpbuf));
                ptmp = strtok_r(tmpbuf, "|", &tok2);
                match = true;
                while ( ptmp != NULL ) {
                    if (!strcmp(ptmp, SaganProcSyslog_LOCAL->syslog_level)) {
                        match = false;
                    }

                    ptmp = strtok_r(NULL, "|", &tok2);
                }
            }

            if ( strcmp(rulestruct[b].s_tag, "" )) {
                strlcpy(tmpbuf, rulestruct[b].s_tag, sizeof(tmpbuf));
                ptmp = strtok_r(tmpbuf, "|", &tok2);
                match = true;
                while ( ptmp != NULL ) {
                    if (!strcmp(ptmp, SaganProcSyslog_LOCAL->syslog_tag)) {
                        match = false;
                    }

                    ptmp = strtok_r(NULL, "|", &tok2);
                }
            }

            /* If there has been a match above,  or NULL on all,  then we continue with
             * PCRE/content search */

            /* Search via strstr (content:) */

            if ( match == false ) {

                if ( rulestruct[b].content_count != 0 ) {

                    for(z=0; z<rulestruct[b].content_count; z++) {


                        /* Content: OFFSET */

                        alter_num = 0;

                        if ( rulestruct[b].s_offset[z] != 0 ) {

                            if ( strlen(SaganProcSyslog_LOCAL->syslog_message) > rulestruct[b].s_offset[z] ) {

                                alter_num = strlen(SaganProcSyslog_LOCAL->syslog_message) - rulestruct[b].s_offset[z];
                                strlcpy(alter_content, SaganProcSyslog_LOCAL->syslog_message + (strlen(SaganProcSyslog_LOCAL->syslog_message) - alter_num), alter_num + 1);

                            } else {

                                alter_content[0] = '\0'; 	/* The offset is larger than the message.  Set content too NULL */

                            }

                        } else {

                            strlcpy(alter_content, SaganProcSyslog_LOCAL->syslog_message, sizeof(alter_content));

                        }

                        /* Content: DEPTH */

                        if ( rulestruct[b].s_depth[z] != 0 ) {

                            /* We do +2 to account for alter_count[0] and whitespace at the begin of syslog message */

                            strlcpy(alter_content, alter_content, rulestruct[b].s_depth[z] + 2);

                        }

                        /* Content: DISTANCE */

                        if ( rulestruct[b].s_distance[z] != 0 ) {

                            alter_num = strlen(SaganProcSyslog_LOCAL->syslog_message) - ( rulestruct[b].s_depth[z-1] + rulestruct[b].s_distance[z] + 1);
                            strlcpy(alter_content, SaganProcSyslog_LOCAL->syslog_message + (strlen(SaganProcSyslog_LOCAL->syslog_message) - alter_num), alter_num + 1);

                            /* Content: WITHIN */

                            if ( rulestruct[b].s_within[z] != 0 ) {
                                strlcpy(alter_content, alter_content, rulestruct[b].s_within[z] + 1);

                            }

                        }

                        /* If case insensitive */

                        if ( rulestruct[b].s_nocase[z] == 1 ) {

                            if (rulestruct[b].content_not[z] != 1 && Sagan_stristr(alter_content, rulestruct[b].s_content[z], false))

                            {
                                sagan_match++;
                            } else {

                                /* for content: ! */

                                if ( rulestruct[b].content_not[z] == 1 && !Sagan_stristr(alter_content, rulestruct[b].s_content[z], false)) sagan_match++;

                            }
                        } else {

                            /* If case sensitive */

                            if ( rulestruct[b].content_not[z] != 1 && Sagan_strstr(alter_content, rulestruct[b].s_content[z] )) {
                                sagan_match++;
                            } else {

                                /* for content: ! */
                                if ( rulestruct[b].content_not[z] == 1 && !Sagan_strstr(alter_content, rulestruct[b].s_content[z])) sagan_match++;

                            }
                        }
                    }
                }

                /* Search via PCRE */

                /* Note:  We verify each "step" has succeeded before function execution.  For example,
                 * if there is a "content",  but that has failed,  there is no point in doing the
                 * pcre or meta_content. */

                if ( rulestruct[b].pcre_count != 0 && sagan_match == rulestruct[b].content_count ) {

                    for(z=0; z<rulestruct[b].pcre_count; z++) {

                        rc = pcre_exec( rulestruct[b].re_pcre[z], rulestruct[b].pcre_extra[z], SaganProcSyslog_LOCAL->syslog_message, (int)strlen(SaganProcSyslog_LOCAL->syslog_message), 0, 0, ovector, PCRE_OVECCOUNT);

                        if ( rc > 0 ) {
                            sagan_match++;
                        }

                    }  /* End of pcre if */
                }

                /* Search via meta_content */

                if ( rulestruct[b].meta_content_count != 0 && sagan_match == rulestruct[b].content_count + rulestruct[b].pcre_count ) {

                    for (z=0; z<rulestruct[b].meta_content_count; z++) {

                        meta_alter_num = 0;

                        /* Meta_content: OFFSET */

                        if ( rulestruct[b].meta_offset[z] != 0 ) {

                            if ( strlen(SaganProcSyslog_LOCAL->syslog_message) > rulestruct[b].meta_offset[z] ) {

                                meta_alter_num = strlen(SaganProcSyslog_LOCAL->syslog_message) - rulestruct[b].meta_offset[z];
                                strlcpy(meta_alter_content, SaganProcSyslog_LOCAL->syslog_message + (strlen(SaganProcSyslog_LOCAL->syslog_message) - meta_alter_num), meta_alter_num + 1);

                            } else {

                                meta_alter_content[0] = '\0';    /* The offset is larger than the message.  Set meta_content too NULL */

                            }

                        } else {

                            strlcpy(meta_alter_content, SaganProcSyslog_LOCAL->syslog_message, sizeof(meta_alter_content));

                        }


                        /* Meta_content: DEPTH */

                        if ( rulestruct[b].meta_depth[z] != 0 ) {

                            /* We do +2 to account for alter_count[0] and whitespace at the begin of syslog message */

                            strlcpy(meta_alter_content, meta_alter_content, rulestruct[b].meta_depth[z] + 2);

                        }

                        /* Meta_content: DISTANCE */

                        if ( rulestruct[b].meta_distance[z] != 0 ) {

                            meta_alter_num = strlen(SaganProcSyslog_LOCAL->syslog_message) - ( rulestruct[b].meta_depth[z-1] + rulestruct[b].meta_distance[z] + 1 );
                            strlcpy(meta_alter_content, SaganProcSyslog_LOCAL->syslog_message + (strlen(SaganProcSyslog_LOCAL->syslog_message) - meta_alter_num), meta_alter_num + 1);

                            /* Meta_ontent: WITHIN */

                            if ( rulestruct[b].meta_within[z] != 0 ) {
                                strlcpy(meta_alter_content, meta_alter_content, rulestruct[b].meta_within[z] + 1);

                            }

                        }

                        rc = Sagan_Meta_Content_Search(meta_alter_content, b, z);

                        if ( rc == 1 ) {
                            sagan_match++;
                        }

                    }
                }


            } /* End of content: & pcre */

            /* if you got match */

            if ( sagan_match == rulestruct[b].pcre_count + rulestruct[b].content_count + rulestruct[b].meta_content_count ) {

                if ( match == false ) {

                    ip_src_flag = 0;
                    ip_dst_flag = 0;

                    normalize_dst_port=0;
                    normalize_src_port=0;
                    normalize_md5_hash[0] = '\0';
                    normalize_sha1_hash[0] = '\0';
                    normalize_sha256_hash[0] = '\0';
                    normalize_filename[0] = '\0';
                    normalize_http_uri[0] = '\0';
                    normalize_http_hostname[0] = '\0';

                    normalize_username[0] = '\0';

#ifdef HAVE_LIBLOGNORM
                    if ( rulestruct[b].normalize == 1 ) {

                        pthread_mutex_lock(&Lognorm_Mutex);

                        liblognorm_status = 0;

                        Sagan_Normalize_Liblognorm(SaganProcSyslog_LOCAL->syslog_message);

                        if (SaganNormalizeLiblognorm->ip_src[0] != '0') {
                            strlcpy(ip_src, SaganNormalizeLiblognorm->ip_src, sizeof(ip_src));
                            ip_src_flag = 1;
                            liblognorm_status = 1;
                        }


                        if (SaganNormalizeLiblognorm->ip_dst[0] != '0' ) {
                            strlcpy(ip_dst, SaganNormalizeLiblognorm->ip_dst, sizeof(ip_dst));
                            ip_dst_flag = 1;
                            liblognorm_status = 1;
                        }

                        if ( SaganNormalizeLiblognorm->src_port != 0 ) {
                            normalize_src_port = SaganNormalizeLiblognorm->src_port;
                            liblognorm_status = 1;
                        }

                        if ( SaganNormalizeLiblognorm->dst_port != 0 ) {
                            normalize_dst_port = SaganNormalizeLiblognorm->dst_port;
                            liblognorm_status = 1;
                        }

                        if ( SaganNormalizeLiblognorm->username[0] != '\0' ) {
                            strlcpy(normalize_username, SaganNormalizeLiblognorm->username, sizeof(normalize_username));
                            liblognorm_status = 1;
                        }

                        if ( SaganNormalizeLiblognorm->http_uri[0] != '\0' ) {
                            strlcpy(normalize_http_uri, SaganNormalizeLiblognorm->http_uri, sizeof(normalize_http_uri));
                            liblognorm_status = 1;
                        }

                        if ( SaganNormalizeLiblognorm->filename[0] != '\0' ) {
                            strlcpy(normalize_filename, SaganNormalizeLiblognorm->filename, sizeof(normalize_filename));
                            liblognorm_status = 1;
                        }


                        if ( SaganNormalizeLiblognorm->hash_sha256[0] != '\0' ) {
                            strlcpy(normalize_sha256_hash, SaganNormalizeLiblognorm->hash_sha256, sizeof(normalize_sha256_hash));
                            liblognorm_status = 1;
                        }


                        if ( SaganNormalizeLiblognorm->hash_sha1[0] != '\0' ) {
                            strlcpy(normalize_sha1_hash, SaganNormalizeLiblognorm->hash_sha1, sizeof(normalize_sha1_hash));
                            liblognorm_status = 1;
                        }

                        if ( SaganNormalizeLiblognorm->hash_md5[0] != '\0' ) {

                            strlcpy(normalize_md5_hash, SaganNormalizeLiblognorm->hash_md5, sizeof(normalize_md5_hash));
                            liblognorm_status = 1;

                        }

                        pthread_mutex_unlock(&Lognorm_Mutex);

                    }

#endif

                    /* Normalization should always over ride parse_src_ip/parse_dst_ip/parse_port,
                     * _unless_ liblognorm fails and both are in a rule */

                    if ( rulestruct[b].normalize == 0 || (rulestruct[b].normalize == 1 && liblognorm_status == 0 ) ) {

                        /* parse_src_ip: {position} */

                        if ( rulestruct[b].s_find_src_ip == 1 ) {
                            strlcpy(ip_src, Sagan_Parse_IP(SaganProcSyslog_LOCAL->syslog_message, rulestruct[b].s_find_src_pos), sizeof(ip_src));
                            ip_src_flag = 1;
                        }

                        /* parse_dst_ip: {postion} */

                        if ( rulestruct[b].s_find_dst_ip == 1 ) {
                            strlcpy(ip_dst, Sagan_Parse_IP(SaganProcSyslog_LOCAL->syslog_message, rulestruct[b].s_find_dst_pos), sizeof(ip_dst));
                            ip_dst_flag = 1;
                        }

                        /* parse_port */

                        if ( rulestruct[b].s_find_port == 1 ) {
                            normalize_src_port = Sagan_Parse_Src_Port(SaganProcSyslog_LOCAL->syslog_message);
                            normalize_dst_port = Sagan_Parse_Dst_Port(SaganProcSyslog_LOCAL->syslog_message);
                        } else {
                            normalize_src_port = config->sagan_port;
                        }

                        /* parse_hash: md5 */

                        if ( rulestruct[b].s_find_hash_type == PARSE_HASH_MD5 ) {
                            strlcpy(normalize_md5_hash, Sagan_Parse_Hash(SaganProcSyslog_LOCAL->syslog_message, PARSE_HASH_MD5), sizeof(normalize_md5_hash));
                        }

                        else if ( rulestruct[b].s_find_hash_type == PARSE_HASH_SHA1 ) {
                            strlcpy(normalize_sha1_hash, Sagan_Parse_Hash(SaganProcSyslog_LOCAL->syslog_message, PARSE_HASH_SHA1), sizeof(normalize_sha1_hash));
                        }

                        else if ( rulestruct[b].s_find_hash_type == PARSE_HASH_SHA256 ) {
                            strlcpy(normalize_sha256_hash, Sagan_Parse_Hash(SaganProcSyslog_LOCAL->syslog_message, PARSE_HASH_SHA256), sizeof(normalize_sha256_hash));
                            printf("-> %s\n", normalize_sha256_hash);
                        }

                        /*  DEBUG
                        else if ( rulestruct[b].s_find_hash_type == PARSE_HASH_ALL )
                            {
                                strlcpy(normalize_sha256_hash, Sagan_Parse_Hash(SaganProcSyslog_LOCAL->syslog_message, PARSE_HASH_SHA256), sizeof(normalize_sha256_hash));
                        }
                        */


                    }


                    /* If the rule calls for proto searching,  we do it now */

                    proto = 0;

                    if ( rulestruct[b].s_find_proto_program == 1 ) {
                        proto = Sagan_Parse_Proto_Program(SaganProcSyslog_LOCAL->syslog_program);
                    }

                    if ( rulestruct[b].s_find_proto == 1 && proto == 0 ) {
                        proto = Sagan_Parse_Proto(SaganProcSyslog_LOCAL->syslog_message);
                    }

                    /* If proto is not searched or has failed,  default to whatever the rule told us to
                       use */

                    if ( proto == 0 ) {
                        proto = rulestruct[b].ip_proto;
                    }

                    if ( ip_src_flag == 0 || ip_src[0] == '0' ) {
                        strlcpy(ip_src, SaganProcSyslog_LOCAL->syslog_host, sizeof(ip_src));
                    }

                    if ( ip_dst_flag == 0 || ip_dst[0] == '0' ) {
                        strlcpy(ip_dst, SaganProcSyslog_LOCAL->syslog_host, sizeof(ip_dst));
                    }

                    if ( normalize_src_port == 0 ) {
                        normalize_src_port=config->sagan_port;
                    }

                    if ( normalize_dst_port == 0 ) {
                        normalize_dst_port=rulestruct[b].dst_port;
                    }

                    if ( proto == 0 ) {
                        proto = config->sagan_proto;		/* Rule didn't specify proto,  use sagan default! */
                    }

                    /* If the "source" is 127.0.0.1 that is not useful.  Replace with config->sagan_host
                     * (defined by user in sagan.conf */

                    if ( !strcmp(ip_src, "127.0.0.1") || !strcmp(ip_src, "::1") ) {
                        strlcpy(ip_src, config->sagan_host, sizeof(ip_src));
                    }

                    if ( !strcmp(ip_dst, "127.0.0.1") || !strcmp(ip_dst, "::1" ) ) {
                        strlcpy(ip_dst, config->sagan_host, sizeof(ip_dst));
                    }

                    ip_src_u32 = IP2Bit(ip_src);
                    ip_dst_u32 = IP2Bit(ip_dst);

                    ip_dstport_u32 = normalize_dst_port;
                    ip_srcport_u32 = normalize_src_port;

                    strlcpy(s_msg, rulestruct[b].s_msg, sizeof(s_msg));

                    /* Check for flow of rule - has_flow is set as rule loading.  It 1, then
                    the rule has some sort of flow.  It 0,  rule is set any/any */

                    if ( rulestruct[b].has_flow == 1 ) {

                        check_flow_return = Sagan_Check_Flow( b, ip_src_u32, ip_dst_u32);

                        if(check_flow_return == false) {

                            counters->follow_flow_drop++;

                        }

                        counters->follow_flow_total++;

                    }

                    /****************************************************************************
                     * Xbit
                     ****************************************************************************/

                    if ( rulestruct[b].xbit_flag && rulestruct[b].xbit_condition_count ) {
                        xbit_return = Sagan_Xbit_Condition(b, ip_src, ip_dst);
                    }

                    /****************************************************************************
                     * Country code
                     ****************************************************************************/

#ifdef HAVE_LIBMAXMINDDB

                    if ( rulestruct[b].geoip2_flag ) {

                        if ( rulestruct[b].geoip2_src_or_dst == 1 ) {
                            geoip2_return = Sagan_GeoIP2_Lookup_Country(ip_src, b);
                        } else {
                            geoip2_return = Sagan_GeoIP2_Lookup_Country(ip_dst, b);
                        }

                        if ( geoip2_return != 2 ) {

                            /* If country IS NOT {my value} return 1 */

                            if ( rulestruct[b].geoip2_type == 1 ) {  		/* isnot */

                                if ( geoip2_return == 1 ) {
                                    geoip2_isset = false;
                                } else {
                                    geoip2_isset = true;
                                    counters->geoip2_hit++;
                                }
                            }

                            /* If country IS {my value} return 1 */

                            if ( rulestruct[b].geoip2_type == 2 ) {           /* is */

                                if ( geoip2_return == 1 ) {
                                    geoip2_isset = true;
                                    counters->geoip2_hit++;
                                } else {
                                    geoip2_isset = false;
                                }
                            }
                        }
                    }

#endif

                    /****************************************************************************
                     * Time based alerting
                     ****************************************************************************/

                    if ( rulestruct[b].alert_time_flag ) {

                        alert_time_trigger = false;

                        if (  Sagan_Check_Time(b) ) {
                            alert_time_trigger = true;
                        }
                    }

                    /****************************************************************************
                     * Blacklist
                     ****************************************************************************/

                    if ( rulestruct[b].blacklist_flag ) {

                        blacklist_results = 0;

                        if ( rulestruct[b].blacklist_ipaddr_src ) {
                            blacklist_results = Sagan_Blacklist_IPADDR( ip_src_u32 );
                        }

                        if ( blacklist_results == 0 && rulestruct[b].blacklist_ipaddr_dst ) {
                            blacklist_results = Sagan_Blacklist_IPADDR( ip_dst_u32 );
                        }

                        if ( blacklist_results == 0 && rulestruct[b].blacklist_ipaddr_all ) {
                            blacklist_results = Sagan_Blacklist_IPADDR_All(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( blacklist_results == 0 && rulestruct[b].blacklist_ipaddr_both ) {
                            if ( Sagan_Blacklist_IPADDR( ip_src_u32 ) || Sagan_Blacklist_IPADDR( ip_dst_u32 ) ) {
                                blacklist_results = 1;
                            }
                        }
                    }

#ifdef WITH_BLUEDOT

                    if ( config->bluedot_flag ) {
                        if ( rulestruct[b].bluedot_ipaddr_type ) {

                            bluedot_results = 0;

                            /* 1 == src,  2 == dst,  3 == both,  4 == all */

                            if ( rulestruct[b].bluedot_ipaddr_type == 1 ) {
                                bluedot_results = Sagan_Bluedot_Lookup(ip_src, BLUEDOT_LOOKUP_IP, b);
                                bluedot_ip_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_IP);
                            }

                            if ( rulestruct[b].bluedot_ipaddr_type == 2 ) {
                                bluedot_results = Sagan_Bluedot_Lookup(ip_dst, BLUEDOT_LOOKUP_IP, b);
                                bluedot_ip_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_IP);
                            }

                            if ( rulestruct[b].bluedot_ipaddr_type == 3 ) {

                                bluedot_results = Sagan_Bluedot_Lookup(ip_src, BLUEDOT_LOOKUP_IP, b);
                                bluedot_ip_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_IP);

                                /* If the source isn't found,  then check the dst */

                                if ( bluedot_ip_flag != 0 ) {
                                    bluedot_results = Sagan_Bluedot_Lookup(ip_dst, BLUEDOT_LOOKUP_IP, b);
                                    bluedot_ip_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_IP);
                                }

                            }

                            if ( rulestruct[b].bluedot_ipaddr_type == 4 ) {

                                bluedot_ip_flag = Sagan_Bluedot_IP_Lookup_All(SaganProcSyslog_LOCAL->syslog_message, b);

                            }

                        }


                        if ( rulestruct[b].bluedot_file_hash && normalize_md5_hash[0] != '\0' ) {

                            bluedot_results = Sagan_Bluedot_Lookup( normalize_md5_hash, BLUEDOT_LOOKUP_HASH, b);
                            bluedot_hash_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_HASH);

                        }

                        if ( rulestruct[b].bluedot_url && normalize_http_uri != '\0' ) {

                            bluedot_results = Sagan_Bluedot_Lookup( normalize_http_uri, BLUEDOT_LOOKUP_URL, b);
                            bluedot_url_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_URL);

                        }

                        if ( rulestruct[b].bluedot_filename && normalize_filename[0] != '\0' ) {

                            bluedot_results = Sagan_Bluedot_Lookup( normalize_filename, BLUEDOT_LOOKUP_FILENAME, b);
                            bluedot_filename_flag = Sagan_Bluedot_Cat_Compare( bluedot_results, b, BLUEDOT_LOOKUP_FILENAME);

                        }

                        /* Do cleanup at the end in case any "hits" above refresh the cache.  This why we don't
                         * "delete" an entry only to re-add it! */

                        Sagan_Bluedot_Check_Cache_Time();


                    }
#endif


                    /****************************************************************************
                    * Bro Intel
                    ****************************************************************************/

                    if ( rulestruct[b].brointel_flag ) {

                        brointel_results = 0;

                        if ( rulestruct[b].brointel_ipaddr_src ) {
                            brointel_results = Sagan_BroIntel_IPADDR( ip_src_u32 );
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_ipaddr_dst ) {
                            brointel_results = Sagan_BroIntel_IPADDR( ip_dst_u32 );
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_ipaddr_all ) {
                            brointel_results = Sagan_BroIntel_IPADDR_All ( SaganProcSyslog_LOCAL->syslog_message );
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_ipaddr_both ) {
                            if ( Sagan_BroIntel_IPADDR( ip_src_u32 ) || Sagan_BroIntel_IPADDR( ip_dst_u32 ) ) {
                                brointel_results = 1;
                            }
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_domain ) {
                            brointel_results = Sagan_BroIntel_DOMAIN(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_file_hash ) {
                            brointel_results = Sagan_BroIntel_FILE_HASH(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_url ) {
                            brointel_results = Sagan_BroIntel_URL(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_software ) {
                            brointel_results = Sagan_BroIntel_SOFTWARE(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_user_name ) {
                            brointel_results = Sagan_BroIntel_USER_NAME(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_file_name ) {
                            brointel_results = Sagan_BroIntel_FILE_NAME(SaganProcSyslog_LOCAL->syslog_message);
                        }

                        if ( brointel_results == 0 && rulestruct[b].brointel_cert_hash ) {
                            brointel_results = Sagan_BroIntel_CERT_HASH(SaganProcSyslog_LOCAL->syslog_message);
                        }

                    }

                    /****************************************************************************/
                    /* Populate the SaganEvent array with the information needed.  This info    */
                    /* will be passed to the threads.  No need to populate it _if_ we're in a   */
                    /* threshold state.                                                         */
                    /****************************************************************************/
                    if ( check_flow_return == true ) {

                        if ( rulestruct[b].xbit_flag == false ||
                             ( rulestruct[b].xbit_flag && rulestruct[b].xbit_set_count && rulestruct[b].xbit_condition_count == 0 ) ||
                             ( rulestruct[b].xbit_flag && rulestruct[b].xbit_set_count && rulestruct[b].xbit_condition_count && xbit_return ) ||
                             ( rulestruct[b].xbit_flag && rulestruct[b].xbit_set_count == 0 && rulestruct[b].xbit_condition_count && xbit_return )) {

                            if ( rulestruct[b].alert_time_flag == 0 || alert_time_trigger == true ) {

#ifdef HAVE_LIBMAXMINDDB
                                if ( rulestruct[b].geoip2_flag == 0 || geoip2_isset == true ) {
#endif
                                    if ( rulestruct[b].blacklist_flag == 0 || blacklist_results == 1 ) {

                                        if ( rulestruct[b].brointel_flag == 0 || brointel_results == 1) {
#ifdef WITH_BLUEDOT


                                            if ( config->bluedot_flag == 0 || rulestruct[b].bluedot_file_hash == 0 || ( rulestruct[b].bluedot_file_hash == 1 && bluedot_hash_flag == 1 )) {

                                                if ( config->bluedot_flag == 0 || rulestruct[b].bluedot_filename == 0 || ( rulestruct[b].bluedot_filename == 1 && bluedot_filename_flag == 1 )) {

                                                    if ( config->bluedot_flag == 0 || rulestruct[b].bluedot_url == 0 || ( rulestruct[b].bluedot_url == 1 && bluedot_url_flag == 1 )) {

                                                        if ( config->bluedot_flag == 0 || rulestruct[b].bluedot_ipaddr_type == 0 || ( rulestruct[b].bluedot_ipaddr_type != 0 && bluedot_ip_flag == 1 )) {



#endif

                                                            after_log_flag = false;

                                                            /*********************************************************/
                                                            /* After - Similar to thresholding,  but the opposite    */
                                                            /* direction - ie - alert _after_ X number of events     */
                                                            /*********************************************************/

                                                            if ( rulestruct[b].after_method != 0 ) {

                                                                after_log_flag = true;

                                                                t = time(NULL);
                                                                now=localtime(&t);
                                                                strftime(timet, sizeof(timet), "%s",  now);

                                                                /* After by source IP address */

                                                                if ( rulestruct[b].after_method == AFTER_BY_SRC ) {

                                                                    after_flag = false;

                                                                    for (i = 0; i < counters_ipc->after_count_by_src; i++ ) {
                                                                        if ( afterbysrc_ipc[i].ipsrc == ip_src_u32  && !strcmp(afterbysrc_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            after_flag = true;

                                                                            Sagan_File_Lock(config->shm_after_by_src);
                                                                            pthread_mutex_lock(&After_By_Src_Mutex);

                                                                            afterbysrc_ipc[i].count++;
                                                                            after_oldtime = atol(timet) - afterbysrc_ipc[i].utime;
                                                                            afterbysrc_ipc[i].utime = atol(timet);

                                                                            if ( after_oldtime > rulestruct[b].after_seconds ) {
                                                                                afterbysrc_ipc[i].count=1;
                                                                                afterbysrc_ipc[i].utime = atol(timet);
                                                                                after_log_flag = true;
                                                                            }

                                                                            pthread_mutex_unlock(&After_By_Src_Mutex);
                                                                            Sagan_File_Unlock(config->shm_after_by_src);

                                                                            if ( rulestruct[b].after_count < afterbysrc_ipc[i].count ) {
                                                                                after_log_flag = false;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "After SID %s by source IP address. [%s]", afterbysrc_ipc[i].sid, ip_src);
                                                                                }


                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->after_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }

                                                                        }
                                                                    }
                                                                }


                                                                /* If not found,  add it to the array */

                                                                if ( after_flag == false ) {

                                                                    if ( Sagan_Clean_IPC_Object(AFTER_BY_SRC) == 0 ) {

                                                                        Sagan_File_Lock(config->shm_after_by_src);
                                                                        pthread_mutex_lock(&After_By_Src_Mutex);

                                                                        afterbysrc_ipc[counters_ipc->after_count_by_src].ipsrc = ip_src_u32;
                                                                        strlcpy(afterbysrc_ipc[counters_ipc->after_count_by_src].sid, rulestruct[b].s_sid, sizeof(afterbysrc_ipc[counters_ipc->after_count_by_src].sid));
                                                                        afterbysrc_ipc[counters_ipc->after_count_by_src].count = 1;
                                                                        afterbysrc_ipc[counters_ipc->after_count_by_src].utime = atol(timet);
                                                                        afterbysrc_ipc[counters_ipc->after_count_by_src].expire = rulestruct[b].after_seconds;

                                                                        Sagan_File_Unlock(config->shm_after_by_src);

                                                                        Sagan_File_Lock(config->shm_counters);

                                                                        counters_ipc->after_count_by_src++;

                                                                        pthread_mutex_unlock(&After_By_Src_Mutex);
                                                                        Sagan_File_Unlock(config->shm_counters);

                                                                    }

                                                                }


                                                                /* After by source IP port */

                                                                if ( rulestruct[b].after_method == 4 ) {

                                                                    after_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->after_count_by_srcport; i++ ) {
                                                                        if ( afterbysrcport_ipc[i].ipsrcport == ip_srcport_u32 && !strcmp(afterbysrcport_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            after_flag = true;

                                                                            Sagan_File_Lock(config->shm_after_by_srcport);
                                                                            pthread_mutex_lock(&After_By_Src_Port_Mutex);

                                                                            afterbysrcport_ipc[i].count++;
                                                                            after_oldtime = atol(timet) - afterbysrcport_ipc[i].utime;
                                                                            afterbysrcport_ipc[i].utime = atol(timet);

                                                                            if ( after_oldtime > rulestruct[b].after_seconds ) {
                                                                                afterbysrcport_ipc[i].count=1;
                                                                                afterbysrcport_ipc[i].utime = atol(timet);
                                                                                after_log_flag = true;
                                                                            }

                                                                            pthread_mutex_unlock(&After_By_Src_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_after_by_srcport);

                                                                            if ( rulestruct[b].after_count < afterbysrcport_ipc[i].count ) {
                                                                                after_log_flag = false;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "After SID %s by source IP port. [%d]", afterbysrcport_ipc[i].sid, ip_srcport_u32);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->after_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( after_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(AFTER_BY_SRCPORT) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_after_by_srcport);
                                                                            pthread_mutex_lock(&After_By_Src_Port_Mutex);

                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].ipsrcport = ip_srcport_u32;
                                                                            strlcpy(afterbysrcport_ipc[counters_ipc->after_count_by_srcport].sid, rulestruct[b].s_sid, sizeof(afterbysrcport_ipc[counters_ipc->after_count_by_srcport].sid));
                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].count = 1;
                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].utime = atol(timet);
                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].expire = rulestruct[b].after_seconds;

                                                                            Sagan_File_Unlock(config->shm_after_by_srcport);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->after_count_by_srcport++;

                                                                            pthread_mutex_unlock(&After_By_Src_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }


                                                                /* After by destination IP address */

                                                                if ( rulestruct[b].after_method == 2 ) {

                                                                    after_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->after_count_by_dst; i++ ) {
                                                                        if ( afterbydst_ipc[i].ipdst == ip_dst_u32 && !strcmp(afterbydst_ipc[i].sid, rulestruct[b].s_sid )) {
                                                                            after_flag = true;

                                                                            Sagan_File_Lock(config->shm_after_by_dst);
                                                                            pthread_mutex_lock(&After_By_Dst_Mutex);

                                                                            afterbydst_ipc[i].count++;
                                                                            after_oldtime = atol(timet) - afterbydst_ipc[i].utime;
                                                                            afterbydst_ipc[i].utime = atol(timet);

                                                                            if ( after_oldtime > rulestruct[b].after_seconds ) {
                                                                                afterbydst_ipc[i].count=1;
                                                                                afterbydst_ipc[i].utime = atol(timet);
                                                                                after_log_flag = true;
                                                                            }

                                                                            pthread_mutex_unlock(&After_By_Dst_Mutex);
                                                                            Sagan_File_Unlock(config->shm_after_by_dst);

                                                                            if ( rulestruct[b].after_count < afterbydst_ipc[i].count ) {
                                                                                after_log_flag = false;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "After SID %s by destination IP address. [%s]", afterbydst_ipc[i].sid, ip_dst);
                                                                                }


                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->after_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( after_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(AFTER_BY_DST) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_after_by_dst);
                                                                            pthread_mutex_lock(&After_By_Dst_Mutex);

                                                                            afterbydst_ipc[counters_ipc->after_count_by_dst].ipdst = ip_dst_u32;
                                                                            strlcpy(afterbydst_ipc[counters_ipc->after_count_by_dst].sid, rulestruct[b].s_sid, sizeof(afterbydst_ipc[counters_ipc->after_count_by_dst].sid));
                                                                            afterbydst_ipc[counters_ipc->after_count_by_dst].count = 1;
                                                                            afterbydst_ipc[counters_ipc->after_count_by_dst].utime = atol(timet);
                                                                            afterbydst_ipc[counters_ipc->after_count_by_dst].expire = rulestruct[b].after_seconds;

                                                                            Sagan_File_Unlock(config->shm_after_by_dst);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->after_count_by_dst++;

                                                                            pthread_mutex_unlock(&After_By_Dst_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                                /* After by source IP port */

                                                                if ( rulestruct[b].after_method == 4 ) {

                                                                    after_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->after_count_by_srcport; i++ ) {
                                                                        if ( afterbysrcport_ipc[i].ipsrcport == ip_srcport_u32 && !strcmp(afterbysrcport_ipc[i].sid, rulestruct[b].s_sid )) {
                                                                            after_flag = true;

                                                                            Sagan_File_Lock(config->shm_after_by_srcport);
                                                                            pthread_mutex_lock(&After_By_Src_Port_Mutex);

                                                                            afterbysrcport_ipc[i].count++;
                                                                            after_oldtime = atol(timet) - afterbysrcport_ipc[i].utime;
                                                                            afterbysrcport_ipc[i].utime = atol(timet);

                                                                            if ( after_oldtime > rulestruct[b].after_seconds ) {
                                                                                afterbysrcport_ipc[i].count=1;
                                                                                afterbysrcport_ipc[i].utime = atol(timet);
                                                                                after_log_flag = true;
                                                                            }

                                                                            pthread_mutex_unlock(&After_By_Src_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_after_by_srcport);

                                                                            if ( rulestruct[b].after_count < afterbysrcport_ipc[i].count ) {
                                                                                after_log_flag = false;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "After SID %s by source IP port. [%d]", afterbysrcport_ipc[i].sid, ip_dstport_u32);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->after_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( after_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(AFTER_BY_SRCPORT) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_after_by_srcport);
                                                                            pthread_mutex_lock(&After_By_Src_Port_Mutex);

                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].ipsrcport = ip_srcport_u32;
                                                                            strlcpy(afterbysrcport_ipc[counters_ipc->after_count_by_srcport].sid, rulestruct[b].s_sid, sizeof(afterbysrcport_ipc[counters_ipc->after_count_by_srcport].sid));
                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].count = 1;
                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].utime = atol(timet);
                                                                            afterbysrcport_ipc[counters_ipc->after_count_by_srcport].expire = rulestruct[b].after_seconds;

                                                                            Sagan_File_Unlock(config->shm_after_by_srcport);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->after_count_by_srcport++;

                                                                            pthread_mutex_unlock(&After_By_Src_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                                /* After by destination IP port */

                                                                if ( rulestruct[b].after_method == 5 ) {

                                                                    after_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->after_count_by_dstport; i++ ) {
                                                                        if ( afterbydstport_ipc[i].ipdstport == ip_dstport_u32 && !strcmp(afterbydstport_ipc[i].sid, rulestruct[b].s_sid )) {
                                                                            after_flag = true;

                                                                            Sagan_File_Lock(config->shm_after_by_dstport);
                                                                            pthread_mutex_lock(&After_By_Dst_Port_Mutex);

                                                                            afterbydstport_ipc[i].count++;
                                                                            after_oldtime = atol(timet) - afterbydstport_ipc[i].utime;
                                                                            afterbydstport_ipc[i].utime = atol(timet);

                                                                            if ( after_oldtime > rulestruct[b].after_seconds ) {
                                                                                afterbydstport_ipc[i].count=1;
                                                                                afterbydstport_ipc[i].utime = atol(timet);
                                                                                after_log_flag = true;
                                                                            }

                                                                            pthread_mutex_unlock(&After_By_Dst_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_after_by_dstport);

                                                                            if ( rulestruct[b].after_count < afterbydstport_ipc[i].count ) {
                                                                                after_log_flag = false;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "After SID %s by destination IP port. [%d]", afterbydstport_ipc[i].sid, ip_dstport_u32);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->after_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( after_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(AFTER_BY_DSTPORT) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_after_by_dstport);
                                                                            pthread_mutex_lock(&After_By_Dst_Port_Mutex);

                                                                            afterbydstport_ipc[counters_ipc->after_count_by_dstport].ipdstport = ip_dstport_u32;
                                                                            strlcpy(afterbydstport_ipc[counters_ipc->after_count_by_dstport].sid, rulestruct[b].s_sid, sizeof(afterbydstport_ipc[counters_ipc->after_count_by_dstport].sid));
                                                                            afterbydstport_ipc[counters_ipc->after_count_by_dstport].count = 1;
                                                                            afterbydstport_ipc[counters_ipc->after_count_by_dstport].utime = atol(timet);
                                                                            afterbydstport_ipc[counters_ipc->after_count_by_dstport].expire = rulestruct[b].after_seconds;

                                                                            Sagan_File_Unlock(config->shm_after_by_dstport);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->after_count_by_dstport++;

                                                                            pthread_mutex_unlock(&After_By_Dst_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                                /* After by username */

                                                                if ( rulestruct[b].after_method == 3 && normalize_username[0] != '\0' ) {

                                                                    after_flag = false;

                                                                    /* Check array for matching username / sid */

                                                                    for (i = 0; i < counters_ipc->after_count_by_username; i++ ) {

                                                                        if ( !strcmp(afterbyusername_ipc[i].username, normalize_username) && !strcmp(afterbyusername_ipc[i].sid, rulestruct[b].s_sid )) {
                                                                            after_flag = true;

                                                                            Sagan_File_Lock(config->shm_after_by_username);
                                                                            pthread_mutex_lock(&After_By_Username_Mutex);

                                                                            afterbyusername_ipc[i].count++;
                                                                            after_oldtime = atol(timet) - afterbyusername_ipc[i].utime;
                                                                            afterbyusername_ipc[i].utime = atol(timet);

                                                                            if ( after_oldtime > rulestruct[b].after_seconds ) {
                                                                                afterbyusername_ipc[i].count=1;
                                                                                afterbyusername_ipc[i].utime = atol(timet);
                                                                                after_log_flag = true;
                                                                            }

                                                                            pthread_mutex_unlock(&After_By_Username_Mutex);
                                                                            Sagan_File_Unlock(config->shm_after_by_username);

                                                                            if ( rulestruct[b].after_count < afterbyusername_ipc[i].count ) {
                                                                                after_log_flag = false;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "After SID %s by_username. [%s]", afterbydst_ipc[i].sid, normalize_username);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->after_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);

                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found, add to the username array */

                                                                    if ( after_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(AFTER_BY_DST) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_after_by_username);
                                                                            pthread_mutex_lock(&After_By_Username_Mutex);

                                                                            strlcpy(afterbyusername_ipc[counters_ipc->after_count_by_username].username, normalize_username, sizeof(afterbyusername_ipc[counters_ipc->after_count_by_username].username));
                                                                            strlcpy(afterbyusername_ipc[counters_ipc->after_count_by_username].sid, rulestruct[b].s_sid, sizeof(afterbyusername_ipc[counters_ipc->after_count_by_username].sid));
                                                                            afterbyusername_ipc[counters_ipc->after_count_by_username].count = 1;
                                                                            afterbyusername_ipc[counters_ipc->after_count_by_username].utime = atol(timet);
                                                                            afterbyusername_ipc[counters_ipc->after_count_by_username].expire = rulestruct[b].after_seconds;

                                                                            Sagan_File_Unlock(config->shm_after_by_username);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->after_count_by_username++;

                                                                            pthread_mutex_unlock(&After_By_Username_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                            } /* End of After */

                                                            thresh_log_flag = false;

                                                            /*********************************************************/
                                                            /* Thresh holding                                        */
                                                            /*********************************************************/

                                                            if ( rulestruct[b].threshold_type != 0 && after_log_flag == false ) {

                                                                t = time(NULL);
                                                                now=localtime(&t);
                                                                strftime(timet, sizeof(timet), "%s",  now);

                                                                /* Thresholding by source IP address */

                                                                if ( rulestruct[b].threshold_method == 1 ) {
                                                                    thresh_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->thresh_count_by_src; i++ ) {
                                                                        if ( threshbysrc_ipc[i].ipsrc == ip_src_u32 && !strcmp(threshbysrc_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            thresh_flag = true;

                                                                            Sagan_File_Lock(config->shm_thresh_by_src);
                                                                            pthread_mutex_lock(&Thresh_By_Src_Mutex);

                                                                            threshbysrc_ipc[i].count++;
                                                                            thresh_oldtime = atol(timet) - threshbysrc_ipc[i].utime;

                                                                            threshbysrc_ipc[i].utime = atol(timet);

                                                                            if ( thresh_oldtime > rulestruct[b].threshold_seconds ) {
                                                                                threshbysrc_ipc[i].count=1;
                                                                                threshbysrc_ipc[i].utime = atol(timet);
                                                                                thresh_log_flag = false;
                                                                            }

                                                                            pthread_mutex_unlock(&Thresh_By_Src_Mutex);
                                                                            Sagan_File_Unlock(config->shm_thresh_by_src);

                                                                            if ( rulestruct[b].threshold_count < threshbysrc_ipc[i].count ) {
                                                                                thresh_log_flag = true;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "Threshold SID %s by source IP address. [%s]", threshbysrc_ipc[i].sid, ip_src);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);
                                                                                counters->threshold_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);

                                                                            }

                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( thresh_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(THRESH_BY_SRC) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_thresh_by_src);
                                                                            pthread_mutex_lock(&Thresh_By_Src_Mutex);

                                                                            threshbysrc_ipc[counters_ipc->thresh_count_by_src].ipsrc = ip_src_u32;
                                                                            strlcpy(threshbysrc_ipc[counters_ipc->thresh_count_by_src].sid, rulestruct[b].s_sid, sizeof(threshbysrc_ipc[counters_ipc->thresh_count_by_src].sid));
                                                                            threshbysrc_ipc[counters_ipc->thresh_count_by_src].count = 1;
                                                                            threshbysrc_ipc[counters_ipc->thresh_count_by_src].utime = atol(timet);
                                                                            threshbysrc_ipc[counters_ipc->thresh_count_by_src].expire = rulestruct[b].threshold_seconds;

                                                                            Sagan_File_Unlock(config->shm_thresh_by_src);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->thresh_count_by_src++;

                                                                            pthread_mutex_unlock(&Thresh_By_Src_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                                /* Thresholding by destination IP address */

                                                                if ( rulestruct[b].threshold_method == 2 ) {
                                                                    thresh_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->thresh_count_by_dst; i++ ) {
                                                                        if ( threshbydst_ipc[i].ipdst == ip_dst_u32 && !strcmp(threshbydst_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            thresh_flag = true;

                                                                            Sagan_File_Lock(config->shm_thresh_by_dst);
                                                                            pthread_mutex_lock(&Thresh_By_Dst_Mutex);

                                                                            threshbydst_ipc[i].count++;
                                                                            thresh_oldtime = atol(timet) - threshbydst_ipc[i].utime;
                                                                            threshbydst_ipc[i].utime = atol(timet);
                                                                            if ( thresh_oldtime > rulestruct[b].threshold_seconds ) {
                                                                                threshbydst_ipc[i].count=1;
                                                                                threshbydst_ipc[i].utime = atol(timet);
                                                                                thresh_log_flag = false;
                                                                            }

                                                                            pthread_mutex_unlock(&Thresh_By_Src_Mutex);
                                                                            Sagan_File_Unlock(config->shm_thresh_by_dst);


                                                                            if ( rulestruct[b].threshold_count < threshbydst_ipc[i].count ) {
                                                                                thresh_log_flag = true;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "Threshold SID %s by destination IP address. [%s]", threshbydst_ipc[i].sid, ip_dst);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);;
                                                                                counters->threshold_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( thresh_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(THRESH_BY_DST) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_thresh_by_dst);
                                                                            pthread_mutex_lock(&Thresh_By_Src_Mutex);

                                                                            threshbydst_ipc[counters_ipc->thresh_count_by_dst].ipdst = ip_dst_u32;
                                                                            strlcpy(threshbydst_ipc[counters_ipc->thresh_count_by_dst].sid, rulestruct[b].s_sid, sizeof(threshbydst_ipc[counters_ipc->thresh_count_by_dst].sid));
                                                                            threshbydst_ipc[counters_ipc->thresh_count_by_dst].count = 1;
                                                                            threshbydst_ipc[counters_ipc->thresh_count_by_dst].utime = atol(timet);
                                                                            threshbydst_ipc[counters_ipc->thresh_count_by_dst].expire = rulestruct[b].threshold_seconds;

                                                                            Sagan_File_Unlock(config->shm_thresh_by_dst);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->thresh_count_by_dst++;

                                                                            pthread_mutex_unlock(&Thresh_By_Src_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                                /* Thresholding by source IP port */

                                                                if ( rulestruct[b].threshold_method == 4 ) {

                                                                    thresh_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->thresh_count_by_srcport; i++ ) {

                                                                        if ( threshbysrcport_ipc[i].ipsrcport == ip_srcport_u32 && !strcmp(threshbysrcport_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            thresh_flag = true;

                                                                            Sagan_File_Lock(config->shm_thresh_by_srcport);
                                                                            pthread_mutex_lock(&Thresh_By_Src_Port_Mutex);

                                                                            threshbysrcport_ipc[i].count++;
                                                                            thresh_oldtime = atol(timet) - threshbysrcport_ipc[i].utime;
                                                                            threshbysrcport_ipc[i].utime = atol(timet);
                                                                            if ( thresh_oldtime > rulestruct[b].threshold_seconds ) {
                                                                                threshbysrcport_ipc[i].count=1;
                                                                                threshbysrcport_ipc[i].utime = atol(timet);
                                                                                thresh_log_flag = false;
                                                                            }

                                                                            pthread_mutex_unlock(&Thresh_By_Src_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_thresh_by_srcport);


                                                                            if ( rulestruct[b].threshold_count < threshbysrcport_ipc[i].count ) {
                                                                                thresh_log_flag = true;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "Threshold SID %s by source IP port. [%s]", threshbydstport_ipc[i].sid, ip_dstport_u32);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);;
                                                                                counters->threshold_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( thresh_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(THRESH_BY_SRCPORT) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_thresh_by_srcport);
                                                                            pthread_mutex_lock(&Thresh_By_Src_Port_Mutex);


                                                                            threshbysrcport_ipc[counters_ipc->thresh_count_by_srcport].ipsrcport = ip_srcport_u32;
                                                                            strlcpy(threshbysrcport_ipc[counters_ipc->thresh_count_by_srcport].sid, rulestruct[b].s_sid, sizeof(threshbysrcport_ipc[counters_ipc->thresh_count_by_srcport].sid));
                                                                            threshbysrcport_ipc[counters_ipc->thresh_count_by_srcport].count = 1;
                                                                            threshbysrcport_ipc[counters_ipc->thresh_count_by_srcport].utime = atol(timet);
                                                                            threshbysrcport_ipc[counters_ipc->thresh_count_by_srcport].expire = rulestruct[b].threshold_seconds;

                                                                            Sagan_File_Unlock(config->shm_thresh_by_srcport);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->thresh_count_by_srcport++;

                                                                            pthread_mutex_unlock(&Thresh_By_Src_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }

                                                                /* Thresholding by destination IP port */

                                                                if ( rulestruct[b].threshold_method == 5 ) {

                                                                    thresh_flag = false;

                                                                    /* Check array for matching src / sid */

                                                                    for (i = 0; i < counters_ipc->thresh_count_by_dstport; i++ ) {
                                                                        if ( threshbydstport_ipc[i].ipdstport == ip_dstport_u32 && !strcmp(threshbydstport_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            thresh_flag = true;

                                                                            Sagan_File_Lock(config->shm_thresh_by_dstport);
                                                                            pthread_mutex_lock(&Thresh_By_Dst_Port_Mutex);

                                                                            threshbydstport_ipc[i].count++;
                                                                            thresh_oldtime = atol(timet) - threshbydstport_ipc[i].utime;
                                                                            threshbydstport_ipc[i].utime = atol(timet);
                                                                            if ( thresh_oldtime > rulestruct[b].threshold_seconds ) {
                                                                                threshbydstport_ipc[i].count=1;
                                                                                threshbydstport_ipc[i].utime = atol(timet);
                                                                                thresh_log_flag = false;
                                                                            }

                                                                            pthread_mutex_unlock(&Thresh_By_Dst_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_thresh_by_dstport);


                                                                            if ( rulestruct[b].threshold_count < threshbydstport_ipc[i].count ) {
                                                                                thresh_log_flag = true;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "Threshold SID %s by destination IP PORT. [%s]", threshbydstport_ipc[i].sid, ip_dstport_u32);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);;
                                                                                counters->threshold_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);
                                                                            }
                                                                        }
                                                                    }

                                                                    /* If not found,  add it to the array */

                                                                    if ( thresh_flag == false ) {

                                                                        if ( Sagan_Clean_IPC_Object(THRESH_BY_DSTPORT) == 0 ) {

                                                                            Sagan_File_Lock(config->shm_thresh_by_dstport);
                                                                            pthread_mutex_lock(&Thresh_By_Dst_Port_Mutex);


                                                                            threshbydstport_ipc[counters_ipc->thresh_count_by_dstport].ipdstport = ip_dstport_u32;
                                                                            strlcpy(threshbydstport_ipc[counters_ipc->thresh_count_by_dstport].sid, rulestruct[b].s_sid, sizeof(threshbydstport_ipc[counters_ipc->thresh_count_by_dstport].sid));
                                                                            threshbydstport_ipc[counters_ipc->thresh_count_by_dstport].count = 1;
                                                                            threshbydstport_ipc[counters_ipc->thresh_count_by_dstport].utime = atol(timet);
                                                                            threshbydstport_ipc[counters_ipc->thresh_count_by_dstport].expire = rulestruct[b].threshold_seconds;

                                                                            Sagan_File_Unlock(config->shm_thresh_by_dstport);

                                                                            Sagan_File_Lock(config->shm_counters);

                                                                            counters_ipc->thresh_count_by_dstport++;

                                                                            pthread_mutex_unlock(&Thresh_By_Dst_Port_Mutex);
                                                                            Sagan_File_Unlock(config->shm_counters);

                                                                        }

                                                                    }
                                                                }


                                                                if ( rulestruct[b].threshold_method == 3 && normalize_username[0] != '\0' ) {

                                                                    thresh_flag = false;

                                                                    /* Check array fror matching username / sid */

                                                                    for (i = 0; i < counters_ipc->thresh_count_by_username; i++) {

                                                                        if ( !strcmp(threshbyusername_ipc[i].username, normalize_username) && !strcmp(threshbyusername_ipc[i].sid, rulestruct[b].s_sid )) {

                                                                            thresh_flag = true;

                                                                            Sagan_File_Lock(config->shm_thresh_by_username);
                                                                            pthread_mutex_lock(&Thresh_By_Username_Mutex);

                                                                            threshbyusername_ipc[i].count++;
                                                                            thresh_oldtime = atol(timet) - threshbyusername_ipc[i].utime;
                                                                            threshbyusername_ipc[i].utime = atol(timet);

                                                                            if ( thresh_oldtime > rulestruct[b].threshold_seconds ) {
                                                                                threshbyusername_ipc[i].count=1;
                                                                                threshbyusername_ipc[i].utime = atol(timet);
                                                                                thresh_log_flag = false;
                                                                            }

                                                                            pthread_mutex_unlock(&Thresh_By_Username_Mutex);
                                                                            Sagan_File_Unlock(config->shm_thresh_by_username);

                                                                            if ( rulestruct[b].threshold_count < threshbyusername_ipc[i].count ) {

                                                                                thresh_log_flag = true;

                                                                                if ( debug->debuglimits ) {
                                                                                    Sagan_Log(S_NORMAL, "Threshold SID %s by_username. [%s]", threshbyusername_ipc[i].sid, normalize_username);
                                                                                }

                                                                                pthread_mutex_lock(&CounterMutex);;
                                                                                counters->threshold_total++;
                                                                                pthread_mutex_unlock(&CounterMutex);

                                                                            }

                                                                        }
                                                                    }

                                                                    /* Username not found, add it to array */

                                                                    if ( thresh_flag == false ) {

                                                                        Sagan_File_Lock(config->shm_thresh_by_username);
                                                                        pthread_mutex_lock(&Thresh_By_Username_Mutex);

                                                                        strlcpy(threshbyusername_ipc[counters_ipc->thresh_count_by_username].username, normalize_username, sizeof(threshbyusername_ipc[counters_ipc->thresh_count_by_username].username));
                                                                        strlcpy(threshbyusername_ipc[counters_ipc->thresh_count_by_username].sid, rulestruct[b].s_sid, sizeof(threshbyusername_ipc[counters_ipc->thresh_count_by_username].sid));
                                                                        threshbyusername_ipc[counters_ipc->thresh_count_by_username].count = 1;
                                                                        threshbyusername_ipc[counters_ipc->thresh_count_by_username].utime = atol(timet);
                                                                        threshbyusername_ipc[counters_ipc->thresh_count_by_username].expire = rulestruct[b].threshold_seconds;


                                                                        Sagan_File_Unlock(config->shm_thresh_by_username);

//                                                                   if ( config->max_threshold_by_username < counters_ipc->thresh_count_by_username ) {
//                                                                        Sagan_Log(S_WARN, "[%s, line %d] Max 'threshold_by_username' of %d has been reached! Consider increasing 'threshold_by_username'!", __FILE__, __LINE__, config->max_threshold_by_username );

//									pthread_mutex_unlock(&Thresh_By_Username_Mutex);


//                                                                    } else {

                                                                        Sagan_File_Lock(config->shm_counters);

                                                                        counters_ipc->thresh_count_by_username++;

                                                                        pthread_mutex_unlock(&Thresh_By_Username_Mutex);
                                                                        Sagan_File_Unlock(config->shm_counters);

//                                                                    }

                                                                    }

                                                                }

                                                            }  /* End of thresholding */


                                                            pthread_mutex_lock(&CounterMutex);
                                                            counters->saganfound++;
                                                            pthread_mutex_unlock(&CounterMutex);

                                                            /* Check for thesholding & "after" */

                                                            if ( thresh_log_flag == false && after_log_flag == false ) {

                                                                if ( debug->debugengine ) {

                                                                    Sagan_Log(S_DEBUG, "[%s, line %d] **[Trigger]*********************************", __FILE__, __LINE__);
                                                                    Sagan_Log(S_DEBUG, "[%s, line %d] Program: %s | Facility: %s | Priority: %s | Level: %s | Tag: %s", __FILE__, __LINE__, SaganProcSyslog_LOCAL->syslog_program, SaganProcSyslog_LOCAL->syslog_facility, SaganProcSyslog_LOCAL->syslog_priority, SaganProcSyslog_LOCAL->syslog_level, SaganProcSyslog_LOCAL->syslog_tag);
                                                                    Sagan_Log(S_DEBUG, "[%s, line %d] Threshold flag: %d | After flag: %d | Xbit Flag: %d | Xbit status: %d", __FILE__, __LINE__, thresh_log_flag, after_log_flag, rulestruct[b].xbit_flag, xbit_return);
                                                                    Sagan_Log(S_DEBUG, "[%s, line %d] Triggering Message: %s", __FILE__, __LINE__, SaganProcSyslog_LOCAL->syslog_message);

                                                                }

                                                                if ( rulestruct[b].xbit_flag && rulestruct[b].xbit_set_count )
                                                                    Sagan_Xbit_Set(b, ip_src, ip_dst);

                                                                threadid++;

                                                                if ( threadid >= MAX_THREADS ) {
                                                                    threadid=0;
                                                                }


                                                                processor_info_engine->processor_name          =       s_msg;
                                                                processor_info_engine->processor_generator_id  =       SAGAN_PROCESSOR_GENERATOR_ID;
                                                                processor_info_engine->processor_facility      =       SaganProcSyslog_LOCAL->syslog_facility;
                                                                processor_info_engine->processor_priority      =       SaganProcSyslog_LOCAL->syslog_level;
                                                                processor_info_engine->processor_pri           =       rulestruct[b].s_pri;
                                                                processor_info_engine->processor_class         =       rulestruct[b].s_classtype;
                                                                processor_info_engine->processor_tag           =       SaganProcSyslog_LOCAL->syslog_tag;
                                                                processor_info_engine->processor_rev           =       rulestruct[b].s_rev;

                                                                processor_info_engine_dst_port                 =       normalize_dst_port;
                                                                processor_info_engine_src_port                 =       normalize_src_port;
                                                                processor_info_engine_proto                    =       proto;
                                                                processor_info_engine_alertid                  =       atoi(rulestruct[b].s_sid);

                                                                if ( rulestruct[b].xbit_flag == false || rulestruct[b].xbit_noalert == 0 ) {

                                                                    if ( rulestruct[b].type == NORMAL_RULE ) {

                                                                        Sagan_Send_Alert(SaganProcSyslog_LOCAL,
                                                                                         processor_info_engine,
                                                                                         ip_src,
                                                                                         ip_dst,
                                                                                         normalize_http_uri,
                                                                                         normalize_http_hostname,
                                                                                         processor_info_engine_proto,
                                                                                         processor_info_engine_alertid,
                                                                                         processor_info_engine_src_port,
                                                                                         processor_info_engine_dst_port,
                                                                                         b );

                                                                    } else {

                                                                        Sagan_Dynamic_Rules(SaganProcSyslog_LOCAL, b, processor_info_engine,
                                                                                            ip_src, ip_dst);

                                                                    }

                                                                }


                                                            } /* Threshold / After */
#ifdef WITH_BLUEDOT
                                                        } /* Bluedot */
                                                    }
                                                }
                                            }
#endif

                                        } /* Bro Intel */

                                    } /* Blacklist */
#ifdef HAVE_LIBMAXMINDDB
                                } /* GeoIP2 */
#endif
                            } /* Time based alerts */

                        } /* Xbit */

                    } /* Check Rule Flow */

                } /* End of match */

            } /* End of pcre match */

#ifdef HAVE_LIBMAXMINDDB
            geoip2_isset = false;
#endif

            match = false;  		      /* Reset match! */
            sagan_match=0;	      /* Reset pcre/meta_content/content match! */
            rc=0;		      /* Return code */
            xbit_return=0;	      /* Xbit reset */
            check_flow_return = true;      /* Rule flow direction reset */


        } /* If normal or dynamic rule */

    } /* End for for loop */

    free(processor_info_engine);

    return(0);
}

