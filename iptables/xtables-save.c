/* Code to save the xtables state, in human readable-form. */
/* (C) 1999 by Paul 'Rusty' Russell <rusty@rustcorp.com.au> and
 * (C) 2000-2002 by Harald Welte <laforge@gnumonks.org>
 * (C) 2012 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This code is distributed under the terms of GNU GPL v2
 *
 */
#include "config.h"
#include <getopt.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>
#include "libiptc/libiptc.h"
#include "iptables.h"
#include "xtables-multi.h"
#include "nft.h"

#include <libnftnl/chain.h>

#ifndef NO_SHARED_LIBS
#include <dlfcn.h>
#endif

#define prog_name xtables_globals.program_name
#define prog_vers xtables_globals.program_version

static bool show_counters = false;

static const struct option options[] = {
	{.name = "counters", .has_arg = false, .val = 'c'},
	{.name = "version",  .has_arg = false, .val = 'V'},
	{.name = "dump",     .has_arg = false, .val = 'd'},
	{.name = "table",    .has_arg = true,  .val = 't'},
	{.name = "modprobe", .has_arg = true,  .val = 'M'},
	{.name = "file",     .has_arg = true,  .val = 'f'},
	{.name = "ipv4",     .has_arg = false, .val = '4'},
	{.name = "ipv6",     .has_arg = false, .val = '6'},
	{NULL},
};

static const struct option arp_save_options[] = {
	{.name = "counters", .has_arg = false, .val = 'c'},
	{.name = "version",  .has_arg = false, .val = 'V'},
	{.name = "modprobe", .has_arg = true,  .val = 'M'},
	{NULL},
};

static const struct option ebt_save_options[] = {
	{.name = "counters", .has_arg = false, .val = 'c'},
	{.name = "version",  .has_arg = false, .val = 'V'},
	{.name = "table",    .has_arg = true,  .val = 't'},
	{.name = "modprobe", .has_arg = true,  .val = 'M'},
	{NULL},
};

static bool ebt_legacy_counter_format;

struct do_output_data {
	bool counters;
};

static int
__do_output(struct nft_handle *h, const char *tablename, void *data)
{
	struct nftnl_chain_list *chain_list;
	struct do_output_data *d = data;

	if (!nft_table_builtin_find(h, tablename))
		return 0;

	if (!nft_is_table_compatible(h, tablename)) {
		printf("# Table `%s' is incompatible, use 'nft' tool.\n",
		       tablename);
		return 0;
	}

	chain_list = nft_chain_list_get(h, tablename);
	if (!chain_list)
		return 0;

	time_t now = time(NULL);

	printf("# Generated by %s v%s on %s", prog_name,
	       prog_vers, ctime(&now));
	printf("*%s\n", tablename);

	/* Dump out chain names first,
	 * thereby preventing dependency conflicts */
	nft_chain_save(h, chain_list);
	nft_rule_save(h, tablename, d->counters ? 0 : FMT_NOCOUNTS);

	now = time(NULL);
	printf("COMMIT\n");
	printf("# Completed on %s", ctime(&now));
	return 0;
}

static int
do_output(struct nft_handle *h, const char *tablename, struct do_output_data *d)
{
	int ret;

	if (!tablename) {
		ret = nft_for_each_table(h, __do_output, d);
		nft_check_xt_legacy(h->family, true);
		return !!ret;
	}

	if (!nft_table_find(h, tablename) &&
	    !nft_table_builtin_find(h, tablename)) {
		fprintf(stderr, "Table `%s' does not exist\n", tablename);
		return 1;
	}

	ret = __do_output(h, tablename, d);
	nft_check_xt_legacy(h->family, true);
	return ret;
}

/* Format:
 * :Chain name POLICY packets bytes
 * rule
 */
static int
xtables_save_main(int family, int argc, char *argv[])
{
	const struct builtin_table *tables;
	const char *tablename = NULL;
	struct do_output_data d = {};
	bool dump = false;
	struct nft_handle h = {
		.family	= family,
	};
	FILE *file = NULL;
	int ret, c;

	xtables_globals.program_name = basename(*argv);;
	c = xtables_init_all(&xtables_globals, family);
	if (c < 0) {
		fprintf(stderr, "%s/%s Failed to initialize xtables\n",
				xtables_globals.program_name,
				xtables_globals.program_version);
		exit(1);
	}

	while ((c = getopt_long(argc, argv, "bcdt:M:f:46V", options, NULL)) != -1) {
		switch (c) {
		case 'b':
			fprintf(stderr, "-b/--binary option is not implemented\n");
			break;
		case 'c':
			d.counters = true;
			break;

		case 't':
			/* Select specific table. */
			tablename = optarg;
			break;
		case 'M':
			xtables_modprobe_program = optarg;
			break;
		case 'f':
			file = fopen(optarg, "w");
			if (file == NULL) {
				fprintf(stderr, "Failed to open file, error: %s\n",
					strerror(errno));
				exit(1);
			}
			ret = dup2(fileno(file), STDOUT_FILENO);
			if (ret == -1) {
				fprintf(stderr, "Failed to redirect stdout, error: %s\n",
					strerror(errno));
				exit(1);
			}
			fclose(file);
			break;
		case 'd':
			dump = true;
			break;
		case '4':
			h.family = AF_INET;
			break;
		case '6':
			h.family = AF_INET6;
			xtables_set_nfproto(AF_INET6);
			break;
		case 'V':
			printf("%s v%s (nf_tables)\n", prog_name, prog_vers);
			exit(0);
		default:
			fprintf(stderr,
				"Look at manual page `%s.8' for more information.\n",
				prog_name);
			exit(1);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unknown arguments found on commandline\n");
		exit(1);
	}

	switch (family) {
	case NFPROTO_IPV4:
	case NFPROTO_IPV6: /* fallthough, same table */
#if defined(ALL_INCLUSIVE) || defined(NO_SHARED_LIBS)
		init_extensions();
		init_extensions4();
#endif
		tables = xtables_ipv4;
		break;
	case NFPROTO_ARP:
		tables = xtables_arp;
		break;
	case NFPROTO_BRIDGE:
		tables = xtables_bridge;
		break;
	default:
		fprintf(stderr, "Unknown family %d\n", family);
		return 1;
	}

	if (nft_init(&h, tables) < 0) {
		fprintf(stderr, "%s/%s Failed to initialize nft: %s\n",
				xtables_globals.program_name,
				xtables_globals.program_version,
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	ret = do_output(&h, tablename, &d);
	nft_fini(&h);
	if (dump)
		exit(0);

	return ret;
}

int xtables_ip4_save_main(int argc, char *argv[])
{
	return xtables_save_main(NFPROTO_IPV4, argc, argv);
}

int xtables_ip6_save_main(int argc, char *argv[])
{
	return xtables_save_main(NFPROTO_IPV6, argc, argv);
}

static int __ebt_save(struct nft_handle *h, const char *tablename, void *data)
{
	struct nftnl_chain_list *chain_list;
	unsigned int format = FMT_NOCOUNTS;
	bool *counters = data;
	time_t now;

	if (!nft_table_find(h, tablename)) {
		printf("Table `%s' does not exist\n", tablename);
		return 1;
	}

	if (!nft_is_table_compatible(h, tablename)) {
		printf("# Table `%s' is incompatible, use 'nft' tool.\n", tablename);
		return 0;
	}

	chain_list = nft_chain_list_get(h, tablename);

	now = time(NULL);
	printf("# Generated by %s v%s on %s", prog_name,
	       prog_vers, ctime(&now));
	printf("*%s\n", tablename);

	if (counters)
		format = FMT_EBT_SAVE |
			(ebt_legacy_counter_format ? FMT_C_COUNTS : 0);

	/* Dump out chain names first,
	 * thereby preventing dependency conflicts */
	nft_chain_save(h, chain_list);
	nft_rule_save(h, tablename, format);
	now = time(NULL);
	printf("# Completed on %s", ctime(&now));
	return 0;
}

static int ebt_save(struct nft_handle *h, const char *tablename, bool counters)
{
	if (!tablename)
		return nft_for_each_table(h, __ebt_save, &counters);

	return __ebt_save(h, tablename, &counters);
}

int xtables_eb_save_main(int argc_, char *argv_[])
{
	const char *ctr = getenv("EBTABLES_SAVE_COUNTER");
	const char *tablename = NULL;
	struct nft_handle h = {
		.family	= NFPROTO_BRIDGE,
	};
	int c;

	if (ctr) {
		if (strcmp(ctr, "yes") == 0) {
			ebt_legacy_counter_format = true;
			show_counters = true;
		}
	}

	xtables_globals.program_name = basename(*argv_);
	c = xtables_init_all(&xtables_globals, h.family);
	if (c < 0) {
		fprintf(stderr, "%s/%s Failed to initialize xtables\n",
				xtables_globals.program_name,
				xtables_globals.program_version);
		exit(1);
	}

	while ((c = getopt_long(argc_, argv_, "ct:M:V", ebt_save_options, NULL)) != -1) {
		switch (c) {
		case 'c':
			unsetenv("EBTABLES_SAVE_COUNTER");
			show_counters = true;
			ebt_legacy_counter_format = false;
			break;
		case 't':
			/* Select specific table. */
			tablename = optarg;
			break;
		case 'M':
			xtables_modprobe_program = optarg;
			break;
		case 'V':
			printf("%s v%s (nf_tables)\n", prog_name, prog_vers);
			exit(0);
		default:
			fprintf(stderr,
				"Look at manual page `%s.8' for more information.\n",
				prog_name);
			exit(1);
		}
	}

	if (nft_init(&h, xtables_bridge) < 0) {
		fprintf(stderr, "%s/%s Failed to initialize nft: %s\n",
				xtables_globals.program_name,
				xtables_globals.program_version,
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	ebt_save(&h, tablename, show_counters);
	nft_fini(&h);
	return 0;
}

int xtables_arp_save_main(int argc, char **argv)
{
	struct nft_handle h = {
		.family	= NFPROTO_ARP,
	};
	time_t now;
	int c;

	xtables_globals.program_name = basename(*argv);;
	c = xtables_init_all(&xtables_globals, h.family);
	if (c < 0) {
		fprintf(stderr, "%s/%s Failed to initialize xtables\n",
				xtables_globals.program_name,
				xtables_globals.program_version);
		exit(1);
	}

	while ((c = getopt_long(argc, argv, "cM:V", arp_save_options, NULL)) != -1) {
		switch (c) {
		case 'c':
			show_counters = true;
			break;
		case 'M':
			xtables_modprobe_program = optarg;
			break;
		case 'V':
			printf("%s v%s (nf_tables)\n", prog_name, prog_vers);
			exit(0);
		default:
			fprintf(stderr,
				"Look at manual page `%s.8' for more information.\n",
				prog_name);
			exit(1);
		}
	}

	if (nft_init(&h, xtables_arp) < 0) {
		fprintf(stderr, "%s/%s Failed to initialize nft: %s\n",
				xtables_globals.program_name,
				xtables_globals.program_version,
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (!nft_table_find(&h, "filter"))
		return 0;

	if (!nft_is_table_compatible(&h, "filter")) {
		printf("# Table `filter' is incompatible, use 'nft' tool.\n");
		return 0;
	}

	printf("# Generated by %s v%s on %s", prog_name,
	       prog_vers, ctime(&now));
	printf("*filter\n");
	nft_chain_save(&h, nft_chain_list_get(&h, "filter"));
	nft_rule_save(&h, "filter", show_counters ? 0 : FMT_NOCOUNTS);
	now = time(NULL);
	printf("# Completed on %s", ctime(&now));
	nft_fini(&h);
	return 0;
}
