/**
 * (C) Copyright 2026 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "place_obj_common.h"
#include "layout_test_helpers.h"

static void
print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("\nOptions:\n");
	printf("  -n, --nodes N         Number of nodes (default: %d)\n", DEFAULT_NODES);
	printf("  -r, --ranks N         Ranks per node (default: %d)\n", DEFAULT_RANKS);
	printf("  -t, --targets N       Targets per rank (default: %d)\n", DEFAULT_TARGETS);
	printf("  -c, --class CLASS     Object class (default: %s)\n", DEFAULT_OBJ_CLASS);
	printf("  -N, --obj-count N     Number of OIDs (default: %d)\n", DEFAULT_OBJ_COUNT);
	printf("  -o, --operation STR   Comma-separated list of operations\n");
	printf("                        Format: <type> <mode>=[<id,...>]\n");
	printf("                        Types : exclude, reint, drain, add\n");
	printf("                        Modes : rank, node, target\n");
	printf("                        Example: \"exclude rank=[0,1], reint node=[2]\"\n");
	printf("  -h, --help            Print this help message\n");
}

static int
parse_int_arg(const char *name, const char *value, int64_t *out)
{
	char     *end = NULL;
	long long parsed;

	errno  = 0;
	parsed = strtoll(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0') {
		fprintf(stderr, "ERROR: %s must be an integer, got \"%s\"\n", name, value);
		*out = -1;
		return -DER_INVAL;
	}

	*out = parsed;
	return 0;
}

static int
get_oclass_ft(daos_oclass_id_t oclass)
{
	struct daos_oclass_attr *attr;
	attr = daos_oclass_id2attr(oclass, 0);
	switch (attr->ca_resil) {
	case DAOS_RES_REPL:
		return attr->u.rp.r_num - 1;
	case DAOS_RES_EC:
		return attr->u.ec.e_p;
	default:
		return 0;
	}
}

static int
parse_operations(char *input, struct operation ops[], int *op_count)
{
	char *pos = input;
	*op_count = 0;

	while (pos != NULL && *pos != '\0') {
		char              type_str[32];
		char              mode_str[32];
		char              arg_str[128];
		char             *arg;
		char             *arg_saveptr = NULL;
		struct operation *op;
		char             *token;
		char             *p;
		int               depth;
		bool              found_sep;

		if (*op_count >= MAX_OPERATIONS) {
			fprintf(stderr, "ERROR: too many operations (max %d)\n", MAX_OPERATIONS);
			return -1;
		}

		while (*pos == ' ')
			pos++;

		if (*pos == '\0')
			break;

		token     = pos;
		depth     = 0;
		found_sep = false;

		/*
		 * Advance pos to the next comma that is NOT inside [...],
		 * so that multi-value args like node=[1,2] are kept intact.
		 */
		for (p = pos; *p != '\0'; p++) {
			if (*p == '[')
				depth++;
			else if (*p == ']')
				depth--;
			else if (*p == ',' && depth == 0) {
				*p        = '\0';
				pos       = p + 1;
				found_sep = true;
				break;
			}
		}
		if (!found_sep)
			pos = NULL; /* last token, no more after this */

		{
			int consumed = 0;

			if (sscanf(token, "%31s %31[^=]=[%127[^]]%n", type_str, mode_str, arg_str,
				   &consumed) != 3) {
				fprintf(stderr,
					"ERROR: invalid operation format: \"%s\"\n"
					"       Expected: <type> <mode>=[<id,...>]\n"
					"       Example : exclude node=[0,1]\n",
					token);
				return -1;
			}

			/* Skip the closing ']' that sscanf left in the stream */
			const char *leftover = token + consumed;

			if (*leftover == ']')
				leftover++;
			while (*leftover == ' ')
				leftover++;
			if (*leftover != '\0') {
				fprintf(stderr,
					"ERROR: operations must be comma-separated, got "
					"space-separated "
					"extra content: \"%s\"\n"
					"       Use commas between operations, e.g.: "
					"\"exclude node=[2], add node=[8]\"\n",
					leftover);
				return -1;
			}
		}

		op = &ops[*op_count];

		/* type */
		if (strcmp(type_str, "exclude") == 0)
			op->type = OP_EXCLUDE;
		else if (strcmp(type_str, "reint") == 0)
			op->type = OP_REINT;
		else if (strcmp(type_str, "drain") == 0)
			op->type = OP_DRAIN;
		else if (strcmp(type_str, "add") == 0)
			op->type = OP_ADD;
		else {
			fprintf(stderr,
				"ERROR: unknown operation type: \"%s\"\n"
				"       Valid types: exclude, reint, drain, add\n",
				type_str);
			return -1;
		}

		/* component */
		if (strcmp(mode_str, "rank") == 0)
			op->component = RANK;
		else if (strcmp(mode_str, "node") == 0)
			op->component = NODE;
		else if (strcmp(mode_str, "target") == 0)
			op->component = TARGET;
		else {
			fprintf(stderr,
				"ERROR: unknown operation component: \"%s\"\n"
				"       Valid components: rank, node, target\n",
				mode_str);
			return -1;
		}

		/* cross-validate type + component */
		if (op->type == OP_ADD && op->component != NODE) {
			fprintf(stderr,
				"ERROR: \"add\" only supports node, got \"%s\"\n"
				"       Usage: add node=[<id,...>]\n",
				mode_str);
			return -1;
		}

		/* args */
		op->nr_args = 0;

		arg = strtok_r(arg_str, ",", &arg_saveptr);

		while (arg != NULL) {
			char  *end = NULL;
			long   val;
			size_t len;

			while (*arg == ' ')
				arg++;
			len = strlen(arg);
			while (len > 0 && arg[len - 1] == ' ') {
				arg[len - 1] = '\0';
				len--;
			}

			if (op->nr_args >= MAX_OP_ARGS)
				return -1;

			errno = 0;
			val   = strtol(arg, &end, 10);
			if (errno != 0 || end == arg || *end != '\0' || val < INT_MIN ||
			    val > INT_MAX) {
				fprintf(stderr,
					"ERROR: invalid operation argument \"%s\"; "
					"must be an integer\n",
					arg);
				return -1;
			}

			op->args[op->nr_args++] = (int)val;

			arg = strtok_r(NULL, ",", &arg_saveptr);
		}

		(*op_count)++;
	}
	return 0;
}

static const char *
op_type_str(enum operation_type type)
{
	switch (type) {
	case OP_EXCLUDE:
		return "exclude";
	case OP_REINT:
		return "reint";
	case OP_DRAIN:
		return "drain";
	case OP_ADD:
		return "add";
	default:
		return "unknown";
	}
}

static const char *
op_component_str(enum operation_component component)
{
	switch (component) {
	case RANK:
		return "rank";
	case NODE:
		return "node";
	case TARGET:
		return "target";
	default:
		return "unknown";
	}
}

/*
 * Execute all operations in sequence. For each operation:
 *   1. Capture the initial layout.
 *   2. Build the target list (by target ID, rank, or node).
 *   3. Apply the two-stage status change and collect the rebuild diff.
 *   4. Capture the post-operation layout.
 *   5. Merge the diff into the initial layout and validate it matches
 *      the post-operation layout; print the moved-shard percentage.
 *
 * @param ctx             Test context (pool/placement maps, OID count, oclass)
 * @param operations      Array of operations to execute
 * @param operation_count Number of operations
 * @return 0 on success, negative DAOS error code on failure
 */
static int
execute_tests(struct test_ctx *ctx, struct operation *operations, int operation_count)
{
	int i;
	int rc = 0;

	for (i = 0; i < operation_count; i++) {
		/*
		 * For OP_ADD/extend the jump map rebalances freely across the
		 * new topology — any number of shards per group can move, so
		 * Check 1 (bounded drift) does not apply.  Pass INT_MAX to
		 * effectively skip it.  Check 2 (diff exactness) still runs.
		 */
		int max_diff =
		    (operations[i].type == OP_ADD) ? INT_MAX : get_oclass_ft(ctx->oclass);
		struct oid_layout         *orig_layout = NULL;
		struct oid_layout         *post_layout = NULL;
		struct shard_diff         *diff        = NULL;
		struct pool_target_id_list tgts        = {0};
		int                        a;

		printf("\n[%d/%d] Running %s %s operation\n", i + 1, operation_count,
		       op_type_str(operations[i].type), op_component_str(operations[i].component));

		do {
			D_ALLOC_ARRAY(orig_layout, ctx->num_oids);
			if (orig_layout == NULL) {
				rc = -DER_NOMEM;
				break;
			}
			D_ALLOC_ARRAY(post_layout, ctx->num_oids);
			if (post_layout == NULL) {
				rc = -DER_NOMEM;
				break;
			}
			D_ALLOC_ARRAY(diff, ctx->num_oids);
			if (diff == NULL) {
				rc = -DER_NOMEM;
				break;
			}

			double ms_place1  = 0.0;
			double ms_rebuild = 0.0;
			double ms_place2  = 0.0;

			/* Capture the initial layout before the operation */
			printf("  Capturing initial layouts...\n");
			rc = capture_layouts(ctx, orig_layout, &ms_place1);
			if (rc != 0) {
				D_ERROR("capture_layouts failed rc=%d\n", rc);
				break;
			}

			/*
			 * For OP_ADD, insert the new node(s) into the pool map
			 * before trying to fetch their targets.
			 */
			if (operations[i].type == OP_ADD) {
				for (a = 0; a < operations[i].nr_args; a++) {
					rc = add_node(ctx, operations[i].args[a]);
					if (rc != 0) {
						D_ERROR("add_node failed rc=%d\n", rc);
						break;
					}
				}
				if (rc != 0)
					break;
			}

			/*
			 * Build the complete target list for this operation.
			 * All component-type logic (TARGET / RANK / NODE) is
			 * handled inside fetch_targets.
			 */
			rc = fetch_targets(ctx, &operations[i], &tgts);
			if (rc != 0) {
				D_ERROR("fetch_targets failed rc=%d\n", rc);
				break;
			}

			/*
			 * Apply the two-stage status change and collect the
			 * per-OID shard rebuild diff between the two stages.
			 */
			printf("  Applying status change and computing diff...\n");
			rc = set_tgt_status_and_find_diff(ctx, &tgts, operations[i].type, diff,
							  &ms_rebuild);
			if (rc != 0) {
				D_ERROR("set_tgt_status_and_find_diff failed rc=%d\n", rc);
				break;
			}

			/* Capture the post-operation layout */
			printf("  Capturing post-operation layouts...\n");
			rc = capture_layouts(ctx, post_layout, &ms_place2);
			if (rc != 0) {
				D_ERROR("capture_layouts failed rc=%d\n", rc);
				break;
			}

			/*
			 * Merge diff into orig_layout, verify it matches
			 * post_layout, and print the moved-shard percentage.
			 */
			printf("  Comparing layouts...\n");
			rc = compare_layout(ctx, orig_layout, diff, post_layout, max_diff);
			if (rc != 0) {
				fprintf(stderr, "  Layout comparison failed rc=%d\n", rc);
				break;
			}

			printf("  Performance (pl_obj_place / pl_obj_find_rebuild only):\n");
			printf("    pl_obj_place   (initial) : %8.3f ms\n", ms_place1);
			printf("    pl_obj_find_rebuild      : %8.3f ms\n", ms_rebuild);
			printf("    pl_obj_place   (post-op) : %8.3f ms\n", ms_place2);
			printf("    Total                    : %8.3f ms\n",
			       ms_place1 + ms_rebuild + ms_place2);
		} while (0);

		pool_target_id_list_free(&tgts);
		free_layouts(orig_layout, ctx->num_oids);
		free_layouts(post_layout, ctx->num_oids);
		D_FREE(diff);

		if (rc != 0)
			return rc;
	}
	return 0;
}

static int
parse_args(int argc, char **argv, int64_t *nodes, int64_t *ranks, int64_t *targets,
	   int64_t *obj_count, char **object_class_str, struct operation operations[],
	   int *operation_count)
{
	bool seen_n = false;
	bool seen_r = false;
	bool seen_t = false;
	bool seen_c = false;
	bool seen_N = false;
	bool seen_o = false;
	int  rc;

	while (1) {
		static struct option long_options[] = {{"nodes", required_argument, 0, 'n'},
						       {"ranks", required_argument, 0, 'r'},
						       {"targets", required_argument, 0, 't'},
						       {"class", required_argument, 0, 'c'},
						       {"obj-count", required_argument, 0, 'N'},
						       {"operation", required_argument, 0, 'o'},
						       {"help", no_argument, 0, 'h'},
						       {0, 0, 0, 0}};

		int                  opt;

		opt = getopt_long(argc, argv, "n:r:t:c:N:o:h", long_options, NULL);
		if (opt == -1)
			break;

#define CHECK_DUPLICATE(flag, name)                                                                \
	do {                                                                                       \
		if (flag) {                                                                        \
			fprintf(stderr, "ERROR: " name " may only be specified once\n");           \
			print_usage(argv[0]);                                                      \
			return -DER_INVAL;                                                         \
		}                                                                                  \
		flag = true;                                                                       \
	} while (0)

		switch (opt) {
		case 'n':
			CHECK_DUPLICATE(seen_n, "-n/--nodes");
			rc = parse_int_arg("nodes", optarg, nodes);
			if (rc != 0)
				return rc;
			break;

		case 'r':
			CHECK_DUPLICATE(seen_r, "-r/--ranks");
			rc = parse_int_arg("ranks", optarg, ranks);
			if (rc != 0)
				return rc;
			break;

		case 't':
			CHECK_DUPLICATE(seen_t, "-t/--targets");
			rc = parse_int_arg("targets", optarg, targets);
			if (rc != 0)
				return rc;
			break;

		case 'c':
			CHECK_DUPLICATE(seen_c, "-c/--class");
			*object_class_str = optarg;
			break;

		case 'N':
			CHECK_DUPLICATE(seen_N, "-N/--obj-count");
			rc = parse_int_arg("obj-count", optarg, obj_count);
			if (rc != 0)
				return rc;
			break;

		case 'o': {
			CHECK_DUPLICATE(seen_o, "-o/--operation");
			char op_buf[1024];

			strncpy(op_buf, optarg, sizeof(op_buf) - 1);
			op_buf[sizeof(op_buf) - 1] = '\0';
			if (parse_operations(op_buf, operations, operation_count) != 0) {
				fprintf(stderr, "ERROR: failed to parse --operation \"%s\"\n",
					optarg);
				return -DER_INVAL;
			}
			break;
		}

		case 'h':
			print_usage(argv[0]);
			return 1;
		default:
			print_usage(argv[0]);
			return -DER_INVAL;
		}
#undef CHECK_DUPLICATE
	}

	if (argc == 1) {
		print_usage(argv[0]);
		return -DER_INVAL;
	}

	return 0;
}

static int
config_setup(struct test_ctx *ctx, int64_t nodes, int64_t ranks, int64_t targets, int64_t obj_count,
	     char *object_class_str, struct operation *operations, int operation_count)
{
	int rc;

	memset(ctx, 0, sizeof(*ctx));
	rc = obj_class_init();
	if (rc != 0) {
		D_ERROR("obj_class_init failed rc=%d\n", rc);
		return rc;
	}
	pl_init();

	rc = validate_configuration(nodes, ranks, targets, obj_count, object_class_str, operations,
				    operation_count);
	if (rc != 0) {
		obj_class_fini();
		pl_fini();
		return rc;
	}

	printf("Running layout test with the following configuration:\n");
	printf("Nodes            : %lld\n", (long long)nodes);
	printf("Ranks per node   : %lld\n", (long long)ranks);
	printf("Targets per rank : %lld\n", (long long)targets);
	printf("Object class     : %s\n", object_class_str);
	printf("Object count     : %lld\n", (long long)obj_count);

	printf("\nSetting up layout...\n");
	ctx->nodes            = (int)nodes;
	ctx->ranks_per_node   = (int)ranks;
	ctx->targets_per_rank = (int)targets;
	ctx->num_oids         = (int)obj_count;
	ctx->oclass           = daos_oclass_name2id(object_class_str);

	uuid_generate(ctx->uuid);
	pool_map_and_pl_map_init(ctx);
	D_ASSERT(ctx->pool_map != NULL);
	D_ASSERT(ctx->pl_map != NULL);

	D_ALLOC_ARRAY(ctx->oids, ctx->num_oids);
	if (ctx->oids == NULL) {
		obj_class_fini();
		cleanup(ctx);
		return -DER_NOMEM;
	}

	rc = generate_oids(ctx);
	if (rc != 0) {
		printf("generate_oids failed rc=%d\n", rc);
		D_FREE(ctx->oids);
		obj_class_fini();
		cleanup(ctx);
		return rc;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int64_t          nodes            = DEFAULT_NODES;
	int64_t          ranks            = DEFAULT_RANKS;
	int64_t          targets          = DEFAULT_TARGETS;
	int64_t          obj_count        = DEFAULT_OBJ_COUNT;
	char            *object_class_str = DEFAULT_OBJ_CLASS;
	struct operation operations[MAX_OPERATIONS];
	int              operation_count = 0;
	int              rc;
	struct test_ctx  ctx;

	rc = parse_args(argc, argv, &nodes, &ranks, &targets, &obj_count, &object_class_str,
			operations, &operation_count);
	if (rc == 1)
		return 0;
	if (rc != 0)
		return EXIT_FAILURE;

	rc = config_setup(&ctx, nodes, ranks, targets, obj_count, object_class_str, operations,
			  operation_count);
	if (rc != 0)
		return EXIT_FAILURE;

	/*
	 * Execute tests
	 */
	rc = execute_tests(&ctx, operations, operation_count);
	if (rc != 0) {
		printf("layout_test_runner failed rc=%d\n", rc);
		D_FREE(ctx.oids);
		obj_class_fini();
		cleanup(&ctx);
		return EXIT_FAILURE;
	}
	printf("\nEnd of test...\n");

	/* Cleanup and finalize */
	D_FREE(ctx.oids);
	obj_class_fini();
	cleanup(&ctx); /* also calls pl_fini() */
	return 0;
}
