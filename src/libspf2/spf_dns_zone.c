/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of either:
 *
 *   a) The GNU Lesser General Public License as published by the Free
 *      Software Foundation; either version 2.1, or (at your option) any
 *      later version,
 *
 *   OR
 *
 *   b) The two-clause BSD license.
 *
 * These licenses can be found with the distribution in the file LICENSES
 */

#include "spf_sys_config.h"

#ifdef STDC_HEADERS
# include <stdio.h>        /* stdin / stdout */
# include <stdlib.h>       /* malloc / free */
#endif


#ifdef HAVE_STRING_H
# include <string.h>       /* strstr / strdup */
#else
# ifdef HAVE_STRINGS_H
#  include <strings.h>       /* strstr / strdup */
# endif
#endif

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#ifdef HAVE_NETDB_H
# include <netdb.h>
#endif


#include "spf.h"
#include "spf_dns.h"
#include "spf_internal.h"
#include "spf_dns_internal.h"
#include "spf_dns_zone.h"


/*
 * this is really little more than a proof-of-concept static zone.
 *
 * The static zone shouldn't just be an unsorted list that must be
 * completely searched each time.  Rather something should be done to
 * allow quicker access.  For example, sorting/bsearch, or red-black
 * trees, or perfect hashes, or something.
 *
 * Note that wildcards mean that a domain could match more than one
 * record.  The most specific record should match.
 *
 * Also, SPF records could be byte-compiled.
 */


typedef struct
{
    SPF_dns_rr_t	**zone;
    int				  num_zone;
    int				  zone_buf_len;
    SPF_dns_rr_t	 *nxdomain;
} SPF_dns_zone_config_t;



static inline SPF_dns_zone_config_t *SPF_voidp2spfhook( void *hook )
    { return (SPF_dns_zone_config_t *)hook; }
static inline void *SPF_spfhook2voidp( SPF_dns_zone_config_t *spfhook )
    { return (void *)spfhook; }




static SPF_dns_rr_t *
SPF_dns_find_zone(SPF_dns_server_t *spf_dns_server,
				const char *domain, ns_type rr_type)
{
    SPF_dns_zone_config_t	*spfhook;
    int		i;

	spfhook = SPF_voidp2spfhook( spf_dns_server->hook );

    if (strncmp(domain, "*.", 2) == 0) {
		for( i = 0; i < spfhook->num_zone; i++ ) {
			if ( spfhook->zone[i]->rr_type == rr_type
			  && strcmp( spfhook->zone[i]->domain, domain ) == 0 )
			return spfhook->zone[i];
		}
    }
	else {
		size_t	domain_len = strlen( domain );

		for( i = 0; i < spfhook->num_zone; i++ ) {
			if ( spfhook->zone[i]->rr_type != rr_type
			  && spfhook->zone[i]->rr_type != ns_t_any )
			continue;

			if ( strncmp( spfhook->zone[i]->domain, "*.", 2 ) == 0 ) {
				size_t	zdomain_len;
				zdomain_len = strlen( spfhook->zone[i]->domain ) - 2;
				if ((zdomain_len <= domain_len)
					 && strcmp(spfhook->zone[i]->domain + 2,
						domain + (domain_len - zdomain_len)) == 0 )
					return spfhook->zone[i];
			}
			else if (strcmp(spfhook->zone[i]->domain, domain) == 0) {
				return spfhook->zone[i];
			}
		}
	}

    return NULL;
}



static SPF_dns_rr_t *
SPF_dns_zone_lookup(SPF_dns_server_t *spf_dns_server,
				const char *domain, ns_type rr_type, int should_cache)
{
    SPF_dns_zone_config_t	*spfhook;
    SPF_dns_rr_t			*spfrr;

    spfrr = SPF_dns_find_zone( spf_dns_server, domain, rr_type );
    if (spfrr) {
		SPF_dns_rr_dup(&spfrr, spfrr);
		return spfrr;
	}

    if (spf_dns_server->layer_below) {
		return SPF_dns_lookup(spf_dns_server->layer_below,
						domain, rr_type, should_cache);
	}

	spfhook = SPF_voidp2spfhook( spf_dns_server->hook );
	SPF_dns_rr_dup(&spfrr, spfhook->nxdomain);

	return spfrr;
}


SPF_errcode_t
SPF_dns_zone_add_str( SPF_dns_server_t *spf_dns_server,
				const char *domain, ns_type rr_type,
				SPF_dns_stat_t herrno, const char *data )
{
    SPF_dns_zone_config_t	*spfhook;
    SPF_dns_rr_t		*spfrr;

    int		err;
    int		cnt;

	spfhook = SPF_voidp2spfhook( spf_dns_server->hook );

    /* try to find an existing record */
    spfrr = SPF_dns_find_zone( spf_dns_server, domain, rr_type );

    /* create a new record */
	if ( spfrr == NULL ) {
		spfrr = SPF_dns_rr_new_init( spf_dns_server,
						domain, rr_type, 24*60*60, herrno );
		if ( spfrr == NULL )
			return SPF_E_NO_MEMORY;

		if ( spfhook->num_zone == spfhook->zone_buf_len ) {
			int				new_len;
			SPF_dns_rr_t	**new_zone;
			int				i;


			new_len = spfhook->zone_buf_len
					+ (spfhook->zone_buf_len >> 2) + 4;
			new_zone = realloc( spfhook->zone,
					new_len * sizeof( *new_zone ) );
			if ( new_zone == NULL )
				return SPF_E_NO_MEMORY;

			for( i = spfhook->zone_buf_len; i < new_len; i++ )
				new_zone[i] = NULL;

			spfhook->zone_buf_len = new_len;
			spfhook->zone = new_zone;
		}


		spfhook->zone[spfhook->num_zone] = spfrr;
		spfhook->num_zone++;

		/* random lexical scope */
		/* Should this really be in this condition? */
		{
			if ( rr_type == ns_t_any ) {
				if ( data )
					SPF_error( "RR type ANY can not have data.");
				if ( herrno == NETDB_SUCCESS )
					SPF_error( "RR type ANY must return a DNS error code.");
			}

			/* We succeeded with the add, but with no data. */
			if ( herrno != NETDB_SUCCESS )
				return SPF_E_SUCCESS;
		}
	}

#define SPF_RR_TRY_REALLOC(rr, i, s) do { \
			SPF_errcode_t __err = SPF_dns_rr_buf_realloc(rr, i, s); \
			if (__err != SPF_E_SUCCESS) return __err; \
		} while(0)

    /*
     * initialize stuff
     */
    cnt = spfrr->num_rr;

	switch( rr_type ) {
	case ns_t_a:
		SPF_RR_TRY_REALLOC(spfrr, cnt, sizeof( spfrr->rr[cnt]->a ));
		err = inet_pton( AF_INET, data, &spfrr->rr[cnt]->a );
		if ( err <= 0 )
			return SPF_E_INVALID_IP4;
		break;

	case ns_t_aaaa:
		SPF_RR_TRY_REALLOC(spfrr, cnt, sizeof( spfrr->rr[cnt]->aaaa ));
		err = inet_pton( AF_INET6, data, &spfrr->rr[cnt]->aaaa );
		if ( err <= 0 )
			return SPF_E_INVALID_IP6;
		break;

	case ns_t_mx:
		SPF_RR_TRY_REALLOC(spfrr, cnt, strlen( data ) + 1);
		strcpy( spfrr->rr[cnt]->mx, data );
		break;

	case ns_t_txt:
		SPF_RR_TRY_REALLOC(spfrr, cnt, strlen( data ) + 1);
		strcpy( spfrr->rr[cnt]->txt, data );
		break;

	case ns_t_ptr:
		SPF_RR_TRY_REALLOC(spfrr, cnt, strlen( data ) + 1);
		strcpy( spfrr->rr[cnt]->ptr, data );
		break;

	case ns_t_any:
		if ( data )
			SPF_error( "RR type ANY can not have data.");
		if ( herrno == NETDB_SUCCESS )
			SPF_error( "RR type ANY must return a DNS error code.");
		SPF_error( "RR type ANY can not have multiple RR.");
		break;

	default:
		SPF_error( "Invalid RR type" );
		break;
	}

    spfrr->num_rr = cnt + 1;

    return SPF_E_SUCCESS;
}



static void
SPF_dns_zone_free( SPF_dns_server_t *spf_dns_server )
{
    SPF_dns_zone_config_t	*spfhook;
    int				i;

	SPF_ASSERT_NOTNULL(spf_dns_server);
	spfhook = SPF_voidp2spfhook( spf_dns_server->hook );

	if (spfhook) {
		if (spfhook->zone) {
			for( i = 0; i < spfhook->zone_buf_len; i++ ) {
				if (spfhook->zone[i])
					SPF_dns_rr_free(spfhook->zone[i]);
			}
			free(spfhook->zone);
		}
		if (spfhook->nxdomain)
			SPF_dns_rr_free(spfhook->nxdomain);
		free(spfhook);
	}

    free(spf_dns_server);
}

SPF_dns_server_t *
SPF_dns_zone_new(SPF_dns_server_t *layer_below,
				const char *name, int debug)
{
	SPF_dns_server_t		*spf_dns_server;
    SPF_dns_zone_config_t	*spfhook;

    spf_dns_server = malloc(sizeof(SPF_dns_server_t));
    if ( spf_dns_server == NULL )
		return NULL;
	memset(spf_dns_server, 0, sizeof(SPF_dns_server_t));

    spf_dns_server->hook = calloc(1, sizeof(SPF_dns_zone_config_t));
    if ( spf_dns_server->hook == NULL ) {
		free( spf_dns_server );
		return NULL;
    }

    if (name ==  NULL)
		name = "resolv";

    spf_dns_server->destroy      = SPF_dns_zone_free;
    spf_dns_server->lookup       = SPF_dns_zone_lookup;
    spf_dns_server->get_spf      = NULL;
    spf_dns_server->get_exp      = NULL;
    spf_dns_server->add_cache    = NULL;
    spf_dns_server->layer_below  = layer_below;
	spf_dns_server->name         = name;
	spf_dns_server->debug        = debug;

    spfhook = SPF_voidp2spfhook(spf_dns_server->hook);

    spfhook->zone_buf_len = 32;
    spfhook->num_zone = 0;
    spfhook->zone = calloc( spfhook->zone_buf_len, sizeof( *spfhook->zone ) );

    if ( spfhook->zone == NULL ) {
		free(spfhook);
		free(spf_dns_server);
		return NULL;
    }

    spfhook->nxdomain = SPF_dns_rr_new_init(spf_dns_server,
					"", ns_t_any, 24 * 60 * 60, HOST_NOT_FOUND);

    return spf_dns_server;
}
