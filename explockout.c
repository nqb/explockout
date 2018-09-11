/* explockout.c - Lock user account until he has waited */
/* an exponential time after failed authentication attempts */
/* $OpenLDAP$ */
/*
 * Copyright 2018 David Coutadeur <david.coutadeur@gmail.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work is loosely derived from the explockout overlay.
 */

#include "portable.h"

/*
 * This file implements an overlay that denies authentication to
 * users who have previously failed to authenticate, requiring them
 * to wait for an exponential time.
 *
 */

#ifdef SLAPD_OVER_EXPLOCKOUT

#include <ldap.h>
#include "lutil.h"
#include "slap.h"
#include <ac/errno.h>
#include <ac/time.h>
#include <ac/string.h>
#include <ac/ctype.h>
#include "config.h"
#include <regex.h>


#define ATTR_NAME_MAX_LEN                  150

/* Per-instance configuration information */
/*
 * if (base time) ^ ( number of pwdFailureTime ) < max time
 *   waiting time = (base time) ^ ( number of pwdFailureTime )
 * if (base time) ^ ( number of pwdFailureTime ) >= max time
 *   waiting time = max time
 */
typedef struct explockout_info {
	/* basetime to compute waiting time */
	int basetime;
	/* maximum waiting time at any time */
	int maxtime;
} explockout_info;


/* configuration attribute and objectclass */
static ConfigTable explockoutcfg[] = {
	{ "explockout-basetime", "seconds", 2, 2, 0,
	  ARG_INT|ARG_OFFSET,
	  (void *)offsetof(explockout_info, basetime),
	  "( OLcfgCtAt:190.1 "
	  "NAME 'olcExpLockoutBaseTime' "
	  "DESC 'base time used for computing exponential lockout waiting time'"
	  "SYNTAX OMsInteger SINGLE-VALUE )", NULL, NULL },

	{ "explockout-maxtime", "seconds", 2, 2, 0,
	  ARG_INT|ARG_OFFSET,
	  (void *)offsetof(explockout_info, maxtime),
	  "( OLcfgCtAt:190.2 "
	  "NAME 'olcExpLockoutMaxTime' "
	  "DESC 'maximum time used for computing exponential lockout waiting time'"
	  "SYNTAX OMsInteger SINGLE-VALUE )", NULL, NULL },

	{ NULL, NULL, 0, 0, 0, ARG_IGNORED }
};

static ConfigOCs explockoutocs[] = {
	{ "( OLcfgCtOc:190.1 "
	  "NAME 'olcExpLockoutConfig' "
	  "DESC 'Exponential lockout configuration' "
	  "SUP olcOverlayConfig "
	  "MAY ( olcExpLockoutBaseTime $ olcExpLockoutMaxTime ) )",
	  Cft_Overlay, explockoutcfg, NULL, NULL },
	{ NULL, 0, NULL }
};

static time_t
parse_time( char *atm )
{
	struct lutil_tm tm;
	struct lutil_timet tt;
	time_t ret = (time_t)-1;

	if ( lutil_parsetime( atm, &tm ) == 0) {
		lutil_tm2time( &tm, &tt );
		ret = tt.tt_sec;
	}
	return ret;
}


int
countPwdFailureTime( Attribute *attrs )
{
    char desc[ATTR_NAME_MAX_LEN];
    int num = 0;
    char *desc_val;
    int desc_len;

    // regex variables
    regex_t regex;
    int reti;
    // Compile regular expression
    reti = regcomp(&regex, "pwdFailureTime", REG_ICASE );

    while( attrs->a_next != NULL )
    {
        desc_val = attrs->a_desc->ad_cname.bv_val;
        desc_len = (int) attrs->a_desc->ad_cname.bv_len;
        strncpy(desc,desc_val,desc_len);
        desc[desc_len]='\0';

        // Execute regular expression
        reti = regexec(&regex, desc, 0, NULL, 0);
        if (!reti)
        {
            // Found pwdFailureTime attribute
            // Count number of values
            num = attrs->a_numvals;
            break;
        }

        attrs = attrs->a_next;
    }

    regfree(&regex);

    Debug( LDAP_DEBUG_ANY, "explockout: Number of failed authentication: %d", num, 0, 0);
    return num;
}

// Get last pwdFailureTime value in user entry
void
getLastPwdFailureTime( Attribute *attrs )
{
    char desc[ATTR_NAME_MAX_LEN];
    char val[15];
    int num = 0;
    char *desc_val;
    int desc_len;
    char *value_val;
    int value_len;
    int i,j;

    // regex variables
    regex_t regex;
    int reti;
    // Compile regular expression
    reti = regcomp(&regex, "pwdFailureTime", REG_ICASE );

    while( attrs->a_next != NULL )
    {
        desc_val = attrs->a_desc->ad_cname.bv_val;
        desc_len = (int) attrs->a_desc->ad_cname.bv_len;
        strncpy(desc,desc_val,desc_len);
        desc[desc_len]='\0';

        // Execute regular expression
        reti = regexec(&regex, desc, 0, NULL, 0);
        if (!reti)
        {
            // Found pwdFailureTime attribute
            // Get all values
            num = attrs->a_numvals;
            for(i=0 ; i< num ; i++)
            {
                value_val = attrs->a_vals[i].bv_val;
                value_len = attrs->a_vals[i].bv_len;
                if(value_len < 14)
                {
                    Debug( LDAP_DEBUG_ANY, "explockout: pwdFailureTime has insufficient digits (14)", 0, 0, 0);
                    exit(EXIT_FAILURE);
                }
                // check if new value is superior
                for(j=0 ; j<14 ; j++)
                {
                    if(value_val[j] > val[j])
                    {
                        strncpy(val,value_val,14);
                        val[14]='\0';
                        break;
                    }
                    else if(value_val[j] < val[j])
                    {
                        break;
                    }
                }
            }
            Debug( LDAP_DEBUG_ANY, "explockout: last failed authentication: %s", val, 0, 0);
            break;
        }

        attrs = attrs->a_next;
    }

    regfree(&regex);

}


static int
explockout_bind_response( Operation *op, SlapReply *rs )
{
	BackendInfo *bi = op->o_bd->bd_info;
	Entry *e;
	int rc;
	int nb_pwdFailureTime = 0;

	/* we're only interested if the bind was successful */
	/*if ( rs->sr_err != LDAP_SUCCESS )
		return SLAP_CB_CONTINUE;*/

	rc = be_entry_get_rw( op, &op->o_req_ndn, NULL, NULL, 0, &e );
	op->o_bd->bd_info = bi;

	if ( rc != LDAP_SUCCESS ) {
		return SLAP_CB_CONTINUE;
	}

	{
		// get configuration parameters
		explockout_info *lbi = (explockout_info *) op->o_callback->sc_private;

		time_t now, bindtime = (time_t)-1;
		Attribute *a;
		Modifications *m;
		char nowstr[ LDAP_LUTIL_GENTIME_BUFSIZE ];
		struct berval timestamp;

		/* get the current time */
		now = slap_get_time();

		Debug( LDAP_DEBUG_ANY, "explockout: basetime: %d\n", lbi->basetime, 0, 0 );
		Debug( LDAP_DEBUG_ANY, "explockout: maxtime: %d\n", lbi->maxtime, 0, 0 );

		nb_pwdFailureTime = countPwdFailureTime( e->e_attrs );

/*
// send deny
send_ldap_error( op, rs, LDAP_UNWILLING_TO_PERFORM,
"operation not allowed within namingContext" );
*/

		/* get authTimestamp attribute, if it exists */
		/*if ((a = attr_find( e->e_attrs, ad_authTimestamp)) != NULL) {
			bindtime = parse_time( a->a_nvals[0].bv_val );

			if (bindtime != (time_t)-1) {
				// if the recorded bind time is within our precision, we're done
				// it doesn't need to be updated (save a write for nothing)
				if ((now - bindtime) < lbi->timestamp_precision) {
					goto done;
				}
			}
		}

		// update the authTimestamp in the user's entry with the current time
		timestamp.bv_val = nowstr;
		timestamp.bv_len = sizeof(nowstr);
		slap_timestamp( &now, &timestamp );

		m = ch_calloc( sizeof(Modifications), 1 );
		m->sml_op = LDAP_MOD_REPLACE;
		m->sml_flags = 0;
		m->sml_type = ad_authTimestamp->ad_cname;
		m->sml_desc = ad_authTimestamp;
		m->sml_numvals = 1;
		m->sml_values = ch_calloc( sizeof(struct berval), 2 );
		m->sml_nvalues = ch_calloc( sizeof(struct berval), 2 );

		ber_dupbv( &m->sml_values[0], &timestamp );
		ber_dupbv( &m->sml_nvalues[0], &timestamp );
		m->sml_next = mod;
		mod = m;*/
	}

done:
	be_entry_release_r( op, e );

	/* perform the update, if necessary */
	/*if ( mod ) {
		Operation op2 = *op;
		SlapReply r2 = { REP_RESULT };
		slap_callback cb = { NULL, slap_null_cb, NULL, NULL };

		// This is a DSA-specific opattr, it never gets replicated.
		op2.o_tag = LDAP_REQ_MODIFY;
		op2.o_callback = &cb;
		op2.orm_modlist = mod;
		op2.o_dn = op->o_bd->be_rootdn;
		op2.o_ndn = op->o_bd->be_rootndn;
		op2.o_dont_replicate = 1;
		rc = op->o_bd->be_modify( &op2, &r2 );
		slap_mods_free( mod, 1 );
	}*/

	op->o_bd->bd_info = bi;
	return SLAP_CB_CONTINUE;
}

static int
explockout_bind( Operation *op, SlapReply *rs )
{
	slap_callback *cb;
	slap_overinst *on = (slap_overinst *) op->o_bd->bd_info;

	/* setup a callback to intercept result of this bind operation
	 * and pass along the explockout_info struct */
	cb = op->o_tmpcalloc( sizeof(slap_callback), 1, op->o_tmpmemctx );
	cb->sc_response = explockout_bind_response;
	cb->sc_next = op->o_callback->sc_next;
	cb->sc_private = on->on_bi.bi_private;
	op->o_callback->sc_next = cb;

	return SLAP_CB_CONTINUE;
}

static int
explockout_db_init(
	BackendDB *be,
	ConfigReply *cr
)
{
	slap_overinst *on = (slap_overinst *) be->bd_info;

	/* initialize private structure to store configuration */
	on->on_bi.bi_private = ch_calloc( 1, sizeof(explockout_info) );

	return 0;
}

static int
explockout_db_close(
	BackendDB *be,
	ConfigReply *cr
)
{
	slap_overinst *on = (slap_overinst *) be->bd_info;
	explockout_info *lbi = (explockout_info *) on->on_bi.bi_private;

	/* free private structure to store configuration */
	free( lbi );

	return 0;
}

static slap_overinst explockout;

int explockout_initialize()
{
	int code;

	// int i;
	/* register operational schema for this overlay (authTimestamp attribute) */
	/*for (i=0; expLockout_OpSchema[i].def; i++) {
		code = register_at( expLockout_OpSchema[i].def, expLockout_OpSchema[i].ad, 0 );
		if ( code ) {
			Debug( LDAP_DEBUG_ANY,
				"explockout_initialize: register_at failed\n", 0, 0, 0 );
			return code;
		}
	}

	ad_authTimestamp->ad_type->sat_flags |= SLAP_AT_MANAGEABLE;*/

	explockout.on_bi.bi_type = "explockout";
	explockout.on_bi.bi_db_init = explockout_db_init;
	explockout.on_bi.bi_db_close = explockout_db_close;
	explockout.on_bi.bi_op_bind = explockout_bind;

	// register configuration directives
	explockout.on_bi.bi_cf_ocs = explockoutocs;
	code = config_register_schema( explockoutcfg, explockoutocs );
	if ( code ) return code;

	return overlay_register( &explockout );
}

#if SLAPD_OVER_EXPLOCKOUT == SLAPD_MOD_DYNAMIC
int init_module(int argc, char *argv[]) {
	return explockout_initialize();
}
#endif

#endif	/* defined(SLAPD_OVER_EXPLOCKOUT) */
