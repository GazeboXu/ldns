/*
 * securechasetrace.c
 * Where all the hard work concerning secure tracing is done
 *
 * (c) 2005, 2006 NLnet Labs
 *
 * See the file LICENSE for the license
 *
 */

#include "drill.h"
#include <ldns/dns.h>

#define SELF "[SELF]"  /* self sig ok */
#define TRUST "[TRUST]"  /* self sig ok */
#define CHAIN "[CHAIN]" /* chain from parent */

/* See if there is a key/ds in trusted that matches
 * a ds in *ds. 
 */
static ldns_rr_list *
ds_key_match(ldns_rr_list *ds, ldns_rr_list *trusted)
{
	size_t i, j;
	bool match;
	ldns_rr *rr_i, *rr_j;
	ldns_rr_list *keys;

	if (!trusted || !ds) {
		return NULL;
	}

	match = false;
	keys = ldns_rr_list_new();
	if (!keys) {
		return NULL;
	}

	if (!ds || !trusted) {
		return NULL;
	}

	for (i = 0; i < ldns_rr_list_rr_count(trusted); i++) {
		rr_i = ldns_rr_list_rr(trusted, i);
		for (j = 0; j < ldns_rr_list_rr_count(ds); j++) {

			rr_j = ldns_rr_list_rr(ds, j);
			if (ldns_rr_compare_ds(rr_i, rr_j)) {
				match = true;
				/* only allow unique RRs to match */
				ldns_rr_set_push_rr(keys, rr_i); 
			}
		}
	}
	if (match) {
		return keys;
	} else {
		return NULL;
	}
}

ldns_pkt *
get_dnssec_pkt(ldns_resolver *r, ldns_rdf *name, ldns_rr_type t) 
{
	ldns_pkt *p = NULL;
	p = ldns_resolver_query(r, name, t, LDNS_RR_CLASS_IN, 0); 
	if (!p) {
		return NULL;
	} else {
		return p;
	}
}

/*
 * generic function to get some RRset from a nameserver
 * and possible some signatures too (that would be the day...)
 */
static ldns_pkt_type
get_dnssec_rr(ldns_pkt *p, ldns_rdf *name, ldns_rr_type t, 
	ldns_rr_list **rrlist, ldns_rr_list **sig)
{
	ldns_pkt_type pt = LDNS_PACKET_UNKNOWN;
	ldns_rr_list *rr = NULL;
	ldns_rr_list *sigs = NULL;

	if (!p) {
		return LDNS_PACKET_UNKNOWN;
	}

	pt = ldns_pkt_reply_type(p);
	if (pt == LDNS_PACKET_NXDOMAIN || pt == LDNS_PACKET_NODATA) {
		return pt;
	}
		
	if (name) {
		rr = ldns_pkt_rr_list_by_name_and_type(p, name, t, LDNS_SECTION_ANSWER);
		sigs = ldns_pkt_rr_list_by_name_and_type(p, name, LDNS_RR_TYPE_RRSIG, 
				LDNS_SECTION_ANSWER);
	} else {
               /* A DS-referral - get the DS records if they are there */
               rr = ldns_pkt_rr_list_by_type(p, t, LDNS_SECTION_AUTHORITY);
               sigs = ldns_pkt_rr_list_by_type(p, LDNS_RR_TYPE_RRSIG,
                               LDNS_SECTION_AUTHORITY);
	}


	if (sig) {
		ldns_rr_list_cat(*sig, sigs);
	}
	if (rrlist) {
		*rrlist = rr;
	}
	return LDNS_PACKET_ANSWER;
}

/* 
 * retrieve keys for this zone
 */
static ldns_pkt_type
get_key(ldns_pkt *p, ldns_rdf *apexname, ldns_rr_list **rrlist, ldns_rr_list **opt_sig)
{
	return get_dnssec_rr(p, apexname, LDNS_RR_TYPE_DNSKEY, rrlist, opt_sig);
}

/*
 * check to see if we can find a DS rrset here which we can then follow
 */
static ldns_pkt_type
get_ds(ldns_pkt *p, ldns_rdf *ownername, ldns_rr_list **rrlist, ldns_rr_list **opt_sig)
{
	return get_dnssec_rr(p, ownername, LDNS_RR_TYPE_DS, rrlist, opt_sig);
}

ldns_pkt *
do_secure_trace(ldns_resolver *local_res, ldns_rdf *name, ldns_rr_type t,
		ldns_rr_class c, ldns_rr_list *trusted_keys)
{
	ldns_resolver *res;
	ldns_pkt *p;
	ldns_rr_list *new_nss_a;
	ldns_rr_list *new_nss_aaaa;
	ldns_rr_list *new_nss;
	ldns_rr_list *ns_addr;
	uint16_t loop_count;
	ldns_rdf *pop; 
	ldns_rdf **labels;
	ldns_status status;
	ssize_t i;
	size_t j;
	uint8_t labels_count;
	ldns_pkt_type pt;

	/* dnssec */
	ldns_rr_list *key_list;
	ldns_rr_list *key_sig_list;
	ldns_rr_list *ds_list;
	ldns_rr_list *ds_sig_list;

	loop_count = 0;
	new_nss_a = NULL;
	new_nss_aaaa = NULL;
	new_nss = NULL;
	ns_addr = NULL;
	key_list = NULL;
	ds_list = NULL;
	pt = LDNS_PACKET_UNKNOWN;

	p = ldns_pkt_new();
	res = ldns_resolver_new();
	key_sig_list = ldns_rr_list_new();
	ds_sig_list = ldns_rr_list_new();

	if (!p || !res) {
		error("Memory allocation failed");
		return NULL;
	}
	
	/* transfer some properties of local_res to res,
	 * because they were given on the commandline */
	ldns_resolver_set_ip6(res, 
			ldns_resolver_ip6(local_res));
	ldns_resolver_set_port(res, 
			ldns_resolver_port(local_res));
	ldns_resolver_set_debug(res, 
			ldns_resolver_debug(local_res));
	ldns_resolver_set_fail(res, 
			ldns_resolver_fail(local_res));
	ldns_resolver_set_usevc(res, 
			ldns_resolver_usevc(local_res));
	ldns_resolver_set_random(res, 
			ldns_resolver_random(local_res));
	ldns_resolver_set_recursive(res, false);
	ldns_resolver_set_dnssec(res, true);

	/* setup the root nameserver in the new resolver */
	if (ldns_resolver_push_nameserver_rr_list(res, global_dns_root) != LDNS_STATUS_OK) {
		return NULL;
	}

	/* this must be a real query to local_res */
	status = ldns_resolver_send(&p, local_res, ldns_dname_new_frm_str("."), LDNS_RR_TYPE_NS, c, 0);
	if (ldns_pkt_empty(p)) {
		warning("No root server information received");
	} 
	
	if (status == LDNS_STATUS_OK) {
		if (!ldns_pkt_empty(p)) {
			drill_pkt_print(stdout, local_res, p);
		}
	} else {
		error("Cannot use local resolver");
		return NULL;
	}

	status = ldns_resolver_send(&p, res, name, t, c, 0);
	if (!p) {
		warning("No packet received, aborting");
		return NULL;
	}

	labels_count = ldns_dname_label_count(name);

	labels = LDNS_XMALLOC(ldns_rdf*, labels_count);
	if (!labels) {
		return NULL;
	}
	labels[0] = name;
	for(i = 1 ; i < (ssize_t)labels_count; i++) {
		labels[i] = ldns_dname_left_chop(labels[i - 1]);
	}

	/* get the nameserver for the label
	 * ask: dnskey and ds for the label 
	 */
	for(i = (ssize_t)labels_count - 1; i >= 0; i--) {

		/* get the nameserver for this label */
		status = ldns_resolver_send(&p, local_res, labels[i], LDNS_RR_TYPE_NS, c, 0);
		new_nss_a = ldns_pkt_rr_list_by_type(p,
				LDNS_RR_TYPE_A, LDNS_SECTION_ADDITIONAL);
		new_nss_aaaa = ldns_pkt_rr_list_by_type(p,
				LDNS_RR_TYPE_AAAA, LDNS_SECTION_ADDITIONAL);
		new_nss = ldns_pkt_rr_list_by_type(p,
				LDNS_RR_TYPE_NS, LDNS_SECTION_AUTHORITY);
		if (!new_nss) {
			/* sometimes they are hidden in the answer section */
			new_nss = ldns_pkt_rr_list_by_type(p,
					LDNS_RR_TYPE_NS, LDNS_SECTION_ANSWER);
		}

		/* remove the old nameserver from the resolver */
		while((pop = ldns_resolver_pop_nameserver(res))) { /* do it */ }

		if (!new_nss_aaaa && !new_nss_a) {
			 /* no nameserver found!!! */
			for(j = 0; j < (ssize_t)ldns_rr_list_rr_count(new_nss); j++) {
				pop = ldns_rr_rdf(ldns_rr_list_rr(new_nss, (size_t)j), 0);
				if (!pop) {
					break;
				}
				/* retrieve it's addresses */
				ns_addr = ldns_rr_list_cat_clone(ns_addr,
					ldns_get_rr_list_addr_by_name(local_res, pop, c, 0));
			}

			if (ns_addr) {
				if (ldns_resolver_push_nameserver_rr_list(res, ns_addr) != 
						LDNS_STATUS_OK) {
					error("Error adding new nameservers");
					ldns_pkt_free(p); 
					return NULL;
				}
				ldns_rr_list_free(ns_addr);
			} else {
				error("Could not find the nameserver ip addr; abort");
				ldns_pkt_free(p);
				return NULL;
			}
		}
		/* add the new ones */
		if (new_nss_aaaa) {
			if (ldns_resolver_push_nameserver_rr_list(res, new_nss_aaaa) != 
					LDNS_STATUS_OK) {
				error("Adding new nameservers");
				ldns_pkt_free(p); 
				return NULL;
			}
		}
		if (new_nss_a) {
			if (ldns_resolver_push_nameserver_rr_list(res, new_nss_a) != 
					LDNS_STATUS_OK) {
				error("Adding new nameservers");
				ldns_pkt_free(p); 
				return NULL;
			}
		}
		
		ldns_rr_list_print(stdout, new_nss);
		puts("");

		p = get_dnssec_pkt(res, labels[i], LDNS_RR_TYPE_DNSKEY);
		pt = get_key(p, labels[i], &key_list, &key_sig_list);
		if (key_list) {
			ldns_rr_list_print(stdout, key_list);
		}
		p = get_dnssec_pkt(res, labels[i], LDNS_RR_TYPE_DS);
		pt = get_ds(p, labels[i], &ds_list, &ds_sig_list);
		if (ds_list) {
			ldns_rr_list_print(stdout, ds_list);
		}
		ds_list = NULL;
		/* we might get lucky and get a DS referral wehn
		 * asking for the key of the query name */
		p = get_dnssec_pkt(res, name, LDNS_RR_TYPE_DNSKEY);
		pt = get_ds(p, NULL, &ds_list, &ds_sig_list); 
		if (ds_list) {
			ldns_rr_list_print(stdout, ds_list);
		}
		ds_list = NULL;

		new_nss_aaaa = NULL;
		new_nss_a = NULL;
		ns_addr = NULL;
		key_list = NULL;
		
		puts("");
	}
	return NULL;
}
