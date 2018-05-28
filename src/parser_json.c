#include <errno.h>
#include <stdint.h> /* needed by gmputil.h */
#include <string.h>
#include <syslog.h>

#include <erec.h>
#include <expression.h>
#include <tcpopt.h>
#include <list.h>
#include <netlink.h>
#include <parser.h>
#include <rule.h>

#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_conntrack_tuple_common.h>
#include <linux/netfilter/nf_log.h>
#include <linux/netfilter/nf_nat.h>
#include <linux/netfilter/nf_tables.h>
#include <jansson.h>

#define CTX_F_RHS	(1 << 0)
#define CTX_F_STMT	(1 << 1)
#define CTX_F_PRIMARY	(1 << 2)
#define CTX_F_DTYPE	(1 << 3)
#define CTX_F_SET_RHS	(1 << 4)
#define CTX_F_MANGLE	(1 << 5)
#define CTX_F_SES	(1 << 6)	/* set_elem_expr_stmt */
#define CTX_F_MAP	(1 << 7)	/* LHS of map_expr */

struct json_ctx {
	struct input_descriptor indesc;
	struct nft_ctx *nft;
	struct list_head *msgs;
	struct list_head *cmds;
	uint32_t flags;
};

#define is_RHS(ctx)	(ctx->flags & CTX_F_RHS)
#define is_STMT(ctx)	(ctx->flags & CTX_F_STMT)
#define is_PRIMARY(ctx)	(ctx->flags & CTX_F_PRIMARY)
#define is_DTYPE(ctx)	(ctx->flags & CTX_F_DTYPE)
#define is_SET_RHS(ctx)	(ctx->flags & CTX_F_SET_RHS)

static char *ctx_flags_to_string(struct json_ctx *ctx)
{
	static char buf[1024];
	const char *sep = "";

	buf[0] = '\0';

	if (is_RHS(ctx)) {
		strcat(buf, sep);
		strcat(buf, "RHS");
		sep = ", ";
	}
	if (is_STMT(ctx)) {
		strcat(buf, sep);
		strcat(buf, "STMT");
		sep = ", ";
	}
	if (is_PRIMARY(ctx)) {
		strcat(buf, sep);
		strcat(buf, "PRIMARY");
		sep = ", ";
	}
	if (is_DTYPE(ctx)) {
		strcat(buf, sep);
		strcat(buf, "DTYPE");
		sep = ", ";
	}
	if (is_SET_RHS(ctx)) {
		strcat(buf, sep);
		strcat(buf, "SET_RHS");
		sep = ", ";
	}
	return buf;
}

/* common parser entry points */

static struct expr *json_parse_expr(struct json_ctx *ctx, json_t *root);
static struct expr *json_parse_rhs_expr(struct json_ctx *ctx, json_t *root);
static struct expr *json_parse_stmt_expr(struct json_ctx *ctx, json_t *root);
static struct expr *json_parse_primary_expr(struct json_ctx *ctx, json_t *root);
static struct expr *json_parse_set_rhs_expr(struct json_ctx *ctx, json_t *root);
static struct expr *json_parse_set_elem_expr_stmt(struct json_ctx *ctx, json_t *root);
static struct expr *json_parse_map_lhs_expr(struct json_ctx *ctx, json_t *root);
static struct stmt *json_parse_stmt(struct json_ctx *ctx, json_t *root);

/* parsing helpers */

const struct location *int_loc = &internal_location;

static void json_lib_error(struct json_ctx *ctx, json_error_t *err)
{
	struct location loc = {
		.indesc = &ctx->indesc,
		.line_offset = err->position - err->column,
		.first_line = err->line,
		.last_line = err->line,
		.first_column = err->column,
		/* no information where problematic part ends :( */
		.last_column = err->column,
	};

	erec_queue(error(&loc, err->text), ctx->msgs);
}

__attribute__((format(printf, 2, 3)))
static void json_error(struct json_ctx *ctx, const char *fmt, ...)
{
	struct error_record *erec;
	va_list ap;

	va_start(ap, fmt);
	erec = erec_vcreate(EREC_ERROR, int_loc, fmt, ap);
	va_end(ap);
	erec_queue(erec, ctx->msgs);
}

static const char *json_typename(const json_t *val)
{
	const char *type_name[] = {
		[JSON_OBJECT] = "object",
		[JSON_ARRAY] = "array",
		[JSON_STRING] = "string",
		[JSON_INTEGER] = "integer",
		[JSON_REAL] = "real",
		[JSON_TRUE] = "true",
		[JSON_FALSE] = "false",
		[JSON_NULL] = "null"
	};

	return type_name[json_typeof(val)];
}

static int json_unpack_err(struct json_ctx *ctx,
			   json_t *root, const char *fmt, ...)
{
	json_error_t err;
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = json_vunpack_ex(root, &err, 0, fmt, ap);
	va_end(ap);

	if (rc)
		json_lib_error(ctx, &err);
	return rc;
}

static int json_unpack_stmt(struct json_ctx *ctx, json_t *root,
			    const char **key, json_t **value)
{
	assert(key);
	assert(value);

	if (json_object_size(root) != 1) {
		json_error(ctx, "Malformed object (too many properties): '%s'.",
			   json_dumps(root, 0));
		return 1;
	}

	json_object_foreach(root, *key, *value)
		return 0;

	/* not reached */
	return 1;
}

static int parse_family(const char *name)
{
	unsigned int i;
	struct {
		const char *name;
		int val;
	} family_tbl[] = {
		{ "ip", NFPROTO_IPV4 },
		{ "ip6", NFPROTO_IPV6 },
		{ "inet", NFPROTO_INET },
		{ "arp", NFPROTO_ARP },
		{ "bridge", NFPROTO_BRIDGE },
		{ "netdev", NFPROTO_NETDEV }
	};

	for (i = 0; i < array_size(family_tbl); i++) {
		if (!strcmp(name, family_tbl[i].name))
			return family_tbl[i].val;
	}
	return -1;
}

static bool is_keyword(const char *keyword)
{
	const char *keywords[] = {
		"ether",
		"ip",
		"ip6",
		"vlan",
		"arp",
		"dnat",
		"snat",
		"ecn",
		"reset",
		"original",
		"reply",
		"label",
	};
	unsigned int i;

	for (i = 0; i < array_size(keywords); i++) {
		if (!strcmp(keyword, keywords[i]))
			return true;
	}
	return false;
}

static bool is_constant(const char *keyword)
{
	const char *constants[] = {
		"tcp",
		"udp",
		"udplite",
		"esp",
		"ah",
		"icmp",
		"icmpv6",
		"comp",
		"dccp",
		"sctp",
		"redirect",
	};
	unsigned int i;

	for (i = 0; i < array_size(constants); i++) {
		if (!strcmp(keyword, constants[i]))
			return true;
	}
	return false;
}

static struct expr *json_parse_constant(struct json_ctx *ctx, const char *name)
{
	const struct {
		const char *name;
		uint8_t data;
		const struct datatype *dtype;
	} constant_tbl[] = {
		{ "tcp", IPPROTO_TCP, &inet_protocol_type },
		{ "udp", IPPROTO_UDP, &inet_protocol_type },
		{ "udplite", IPPROTO_UDPLITE, &inet_protocol_type },
		{ "esp", IPPROTO_ESP, &inet_protocol_type },
		{ "ah", IPPROTO_AH, &inet_protocol_type },
		{ "icmp", IPPROTO_ICMP, &inet_protocol_type },
		{ "icmpv6", IPPROTO_ICMPV6, &inet_protocol_type },
		{ "comp", IPPROTO_COMP, &inet_protocol_type },
		{ "dccp", IPPROTO_DCCP, &inet_protocol_type },
		{ "sctp", IPPROTO_SCTP, &inet_protocol_type },
		{ "redirect", ICMP_REDIRECT, &icmp_type_type },
	};
	unsigned int i;

	for (i = 0; i < array_size(constant_tbl); i++) {
		if (strcmp(name, constant_tbl[i].name))
			continue;
		return constant_expr_alloc(int_loc,
					   constant_tbl[i].dtype,
					   BYTEORDER_HOST_ENDIAN,
					   8 * BITS_PER_BYTE,
					   &constant_tbl[i].data);
	}
	json_error(ctx, "Unknown constant '%s'.", name);
	return NULL;
}

/* this is a combination of symbol_expr, integer_expr, boolean_expr ... */
static struct expr *json_parse_immediate_expr(struct json_ctx *ctx,
					      const char *type, json_t *root)
{
	enum symbol_types symtype = SYMBOL_VALUE;
	const char *str;
	char buf[64] = {};
	struct expr;

	switch (json_typeof(root)) {
	case JSON_STRING:
		str = json_string_value(root);
		if (str[0] == '@') {
			symtype = SYMBOL_SET;
			str++;
		}
		if (is_RHS(ctx) && is_keyword(str))
			return symbol_expr_alloc(int_loc,
						 SYMBOL_VALUE, NULL, str);
		if (is_RHS(ctx) && is_constant(str))
			return json_parse_constant(ctx, str);
		break;
	case JSON_INTEGER:
		snprintf(buf, sizeof(buf),
			 "%" JSON_INTEGER_FORMAT, json_integer_value(root));
		str = buf;
		break;
	case JSON_TRUE:
	case JSON_FALSE:
		if (is_RHS(ctx)) {
			buf[0] = json_is_true(root);
			return constant_expr_alloc(int_loc, &boolean_type,
						   BYTEORDER_HOST_ENDIAN,
						   1, buf);
		}
		/* fall through */
	default:
		json_error(ctx, "Invalid immediate value type '%d'.",
			   json_typeof(root));
		return NULL;
	}

	return symbol_expr_alloc(int_loc, symtype, NULL, str);
}

static struct expr *json_parse_meta_expr(struct json_ctx *ctx,
					 const char *type, json_t *root)
{
	struct error_record *erec;
	unsigned int key;
	const char *name;

	if (json_unpack_err(ctx, root, "s", &name))
		return NULL;
	erec = meta_key_parse(int_loc, name, &key);
	if (erec) {
		erec_queue(erec, ctx->msgs);
		return NULL;
	}
	return meta_expr_alloc(int_loc, key);
}

static int json_parse_payload_field(const struct proto_desc *desc,
				    const char *name, int *field)
{
	unsigned int i;

	for (i = 0; i < PROTO_HDRS_MAX; i++) {
		if (desc->templates[i].token &&
		    !strcmp(desc->templates[i].token, name)) {
			if (field)
				*field = i;
			return 0;
		}
	}
	return 1;
}

static int json_parse_tcp_option_type(const char *name, int *val)
{
	unsigned int i;

	for (i = 0; i < array_size(tcpopthdr_protocols); i++) {
		if (tcpopthdr_protocols[i] &&
		    !strcmp(tcpopthdr_protocols[i]->name, name)) {
			if (val)
				*val = i;
			return 0;
		}
	}
	/* special case for sack0 - sack3 */
	if (sscanf(name, "sack%u", &i) == 1 && i < 4) {
		if (val)
			*val = TCPOPTHDR_SACK0 + i;
		return 0;
	}
	return 1;
}

static int json_parse_tcp_option_field(int type, const char *name, int *val)
{
	unsigned int i;
	const struct exthdr_desc *desc = tcpopthdr_protocols[type];

	for (i = 0; i < array_size(desc->templates); i++) {
		if (desc->templates[i].token &&
		    !strcmp(desc->templates[i].token, name)) {
			if (val)
				*val = i;
			return 0;
		}
	}
	return 1;
}

static const struct proto_desc *proto_lookup_byname(const char *name)
{
	const struct proto_desc *proto_tbl[] = {
		&proto_eth,
		&proto_vlan,
		&proto_arp,
		&proto_ip,
		&proto_icmp,
		&proto_ip6,
		&proto_icmp6,
		&proto_ah,
		&proto_esp,
		&proto_comp,
		&proto_udp,
		&proto_udplite,
		&proto_tcp,
		&proto_dccp,
		&proto_sctp
	};
	unsigned int i;

	for (i = 0; i < array_size(proto_tbl); i++) {
		if (!strcmp(proto_tbl[i]->name, name))
			return proto_tbl[i];
	}
	return NULL;
}

static struct expr *json_parse_payload_expr(struct json_ctx *ctx,
					    const char *type, json_t *root)
{
	const char *name;
	const char *field;
	int val;
	const struct proto_desc *proto;

	if (json_unpack_err(ctx, root, "{s:s}", "name", &name))
		return NULL;

	/* special treatment for raw */

	if (!strcmp(name, "raw")) {
		int offset, len, baseval;
		struct expr *expr;
		const char *base;

		if (json_unpack_err(ctx, root, "{s:s, s:i, s:i}",
				    "base", &base,
				    "offset", &offset,
				    "len", &len))
			return NULL;
		if (!strcmp(base, "ll")) {
			baseval = PROTO_BASE_LL_HDR;
		} else if (!strcmp(base, "nh")) {
			baseval = PROTO_BASE_NETWORK_HDR;
		} else if (!strcmp(base, "th")) {
			baseval = PROTO_BASE_TRANSPORT_HDR;
		} else {
			json_error(ctx, "Invalid payload base '%s'.", base);
			return NULL;
		}
		expr = payload_expr_alloc(int_loc, NULL, 0);
		payload_init_raw(expr, baseval, offset, len);
		expr->byteorder		= BYTEORDER_BIG_ENDIAN;
		expr->payload.is_raw	= true;

		return expr;
	}

	proto = proto_lookup_byname(name);
	if (!proto) {
		json_error(ctx, "Unknown payload expr name '%s'.", name);
		return NULL;
	}
	if (json_unpack_err(ctx, root, "{s:s}", "field", &field))
		return NULL;
	if (json_parse_payload_field(proto, field, &val)) {
		json_error(ctx, "Unknown %s field '%s'.", name, field);
		return NULL;
	}
	return payload_expr_alloc(int_loc, proto, val);
}

static struct expr *json_parse_tcp_option_expr(struct json_ctx *ctx,
					       const char *type, json_t *root)
{
	const char *desc, *field = NULL;
	int descval, fieldval;
	struct expr *expr;

	if (json_unpack_err(ctx, root, "{s:s}", "name", &desc))
		return NULL;
	json_unpack(root, "{s:s}", "field", &field);

	if (json_parse_tcp_option_type(desc, &descval)) {
		json_error(ctx, "Unknown tcp option name '%s'.", desc);
		return NULL;
	}

	if (!field) {
		expr = tcpopt_expr_alloc(int_loc, descval,
					 TCPOPTHDR_FIELD_KIND);
		expr->exthdr.flags = NFT_EXTHDR_F_PRESENT;

		return expr;
	}
	if (json_parse_tcp_option_field(descval, field, &fieldval)) {
		json_error(ctx, "Unknown tcp option field '%s'.", field);
		return NULL;
	}
	return tcpopt_expr_alloc(int_loc, descval, fieldval);
}

static const struct exthdr_desc *exthdr_lookup_byname(const char *name)
{
	const struct exthdr_desc *exthdr_tbl[] = {
		&exthdr_hbh,
		&exthdr_rt,
		&exthdr_rt0,
		&exthdr_rt2,
		&exthdr_rt4,
		&exthdr_frag,
		&exthdr_dst,
		&exthdr_mh,
	};
	unsigned int i;

	for (i = 0; i < array_size(exthdr_tbl); i++) {
		if (!strcmp(exthdr_tbl[i]->name, name))
			return exthdr_tbl[i];
	}
	return NULL;
}

static int json_parse_exthdr_field(const struct exthdr_desc *desc,
				   const char *name, int *field)
{
	unsigned int i;

	for (i = 0; i < array_size(desc->templates); i++) {
		if (desc->templates[i].token &&
		    !strcmp(desc->templates[i].token, name)) {
			if (field)
				*field = i;
			return 0;
		}
	}
	return 1;
}

static struct expr *json_parse_exthdr_expr(struct json_ctx *ctx,
					   const char *type, json_t *root)
{
	const char *name, *field = NULL;
	struct expr *expr;
	int offset = 0, fieldval;
	const struct exthdr_desc *desc;

	if (json_unpack_err(ctx, root, "{s:s}", "name", &name))
		return NULL;

	desc = exthdr_lookup_byname(name);
	if (!desc) {
		json_error(ctx, "Invalid exthdr protocol '%s'.", name);
		return NULL;
	}

	if (json_unpack(root, "{s:s}", "field", &field)) {
		expr = exthdr_expr_alloc(int_loc, desc, 1);
		expr->exthdr.flags = NFT_EXTHDR_F_PRESENT;
		return expr;
	}

	if (json_parse_exthdr_field(desc, field, &fieldval)) {
		json_error(ctx, "Unknown %s field %s.", desc->name, field);
		return NULL;
	}

	/* special treatment for rt0 */
	if (desc == &exthdr_rt0 &&
	    json_unpack_err(ctx, root, "{s:i}", "offset", &offset))
		return NULL;

	return exthdr_expr_alloc(int_loc, desc, fieldval + offset);
}

static struct expr *json_parse_rt_expr(struct json_ctx *ctx,
				       const char *type, json_t *root)
{
	const struct {
		const char *name;
		int val;
	} rt_key_tbl[] = {
		{ "classid", NFT_RT_CLASSID },
		{ "nexthop", NFT_RT_NEXTHOP4 },
		{ "mtu", NFT_RT_TCPMSS },
	};
	unsigned int i, familyval = NFPROTO_UNSPEC;
	const char *key, *family = NULL;

	if (json_unpack_err(ctx, root, "{s:s}", "key", &key))
		return NULL;
	if (!json_unpack(root, "{s:s}", "family", &family)) {
		familyval = parse_family(family);
		if (familyval != NFPROTO_IPV4 &&
		    familyval != NFPROTO_IPV6) {
			json_error(ctx, "Invalid RT family '%s'.", family);
			return NULL;
		}
	}

	for (i = 0; i < array_size(rt_key_tbl); i++) {
		int val = rt_key_tbl[i].val;
		bool invalid = true;

		if (strcmp(key, rt_key_tbl[i].name))
			continue;

		if (familyval) {
			if (familyval == NFPROTO_IPV6 &&
			    val == NFT_RT_NEXTHOP4)
				val = NFT_RT_NEXTHOP6;
			invalid = false;
		}
		return rt_expr_alloc(int_loc, val, invalid);
	}
	json_error(ctx, "Unknown rt key '%s'.", key);
	return NULL;
}

static bool ct_key_is_dir(enum nft_ct_keys key)
{
	const enum nft_ct_keys ct_dir_keys[] = {
		NFT_CT_L3PROTOCOL,
		NFT_CT_SRC,
		NFT_CT_DST,
		NFT_CT_PROTOCOL,
		NFT_CT_PROTO_SRC,
		NFT_CT_PROTO_DST,
		NFT_CT_PKTS,
		NFT_CT_BYTES,
		NFT_CT_AVGPKT,
		NFT_CT_ZONE,
	};
	unsigned int i;

	for (i = 0; i < array_size(ct_dir_keys); i++) {
		if (key == ct_dir_keys[i])
			return true;
	}
	return false;
}

static struct expr *json_parse_ct_expr(struct json_ctx *ctx,
				       const char *type, json_t *root)
{
	const char *key, *dir, *family;
	unsigned int i;
	int dirval = -1, familyval = NFPROTO_UNSPEC, keyval = -1;

	if (json_unpack_err(ctx, root, "{s:s}", "key", &key))
		return NULL;

	for (i = 0; i < array_size(ct_templates); i++) {
		if (ct_templates[i].token &&
		    !strcmp(key, ct_templates[i].token)) {
			keyval = i;
			break;
		}
	}
	if (keyval == -1) {
		json_error(ctx, "Unknown ct key '%s'.", key);
		return NULL;
	}

	if (!json_unpack(root, "{s:s}", "family", &family)) {
		familyval = parse_family(family);
		if (familyval != NFPROTO_IPV4 &&
		    familyval != NFPROTO_IPV6) {
			json_error(ctx, "Invalid CT family '%s'.", family);
			return NULL;
		}
	}

	if (!json_unpack(root, "{s:s}", "dir", &dir)) {
		if (!strcmp(dir, "original")) {
			dirval = IP_CT_DIR_ORIGINAL;
		} else if (!strcmp(dir, "reply")) {
			dirval = IP_CT_DIR_REPLY;
		} else {
			json_error(ctx, "Invalid ct direction '%s'.", dir);
			return NULL;
		}

		if (!ct_key_is_dir(keyval)) {
			json_error(ctx, "Direction not supported by CT key '%s'.", key);
			return NULL;
		}
	}

	return ct_expr_alloc(int_loc, keyval, dirval, familyval);
}

static struct expr *json_parse_numgen_expr(struct json_ctx *ctx,
					   const char *type, json_t *root)
{
	int modeval, mod, offset = 0;
	const char *mode;

	if (json_unpack_err(ctx, root, "{s:s, s:i}",
			    "mode", &mode, "mod", &mod))
		return NULL;
	json_unpack(root, "{s:i}", "offset", &offset);

	if (!strcmp(mode, "inc")) {
		modeval = NFT_NG_INCREMENTAL;
	} else if (!strcmp(mode, "random")) {
		modeval = NFT_NG_RANDOM;
	} else {
		json_error(ctx, "Unknown numgen mode '%s'.", mode);
		return NULL;
	}

	return numgen_expr_alloc(int_loc, modeval, mod, offset);
}

static struct expr *json_parse_hash_expr(struct json_ctx *ctx,
					 const char *type, json_t *root)
{
	int mod, offset = 0, seed = 0;
	struct expr *expr, *hash_expr;
	bool have_seed;
	json_t *jexpr;


	if (json_unpack_err(ctx, root, "{s:i}", "mod", &mod))
		return NULL;
	json_unpack(root, "{s:i}", "offset", &offset);

	if (!strcmp(type, "symhash")) {
		return hash_expr_alloc(int_loc, mod, false, 0,
				       offset, NFT_HASH_SYM);
	} else if (strcmp(type, "jhash")) {
		json_error(ctx, "Unknown hash type '%s'.", type);
		return NULL;
	}

	if (json_unpack_err(ctx, root, "{s:o}", "expr", &jexpr))
		return NULL;
	expr = json_parse_expr(ctx, jexpr);
	if (!expr) {
		json_error(ctx, "Invalid jhash expression.");
		return NULL;
	}
	have_seed = !json_unpack(root, "{s:i}", "seed", &seed);

	hash_expr = hash_expr_alloc(int_loc, mod, have_seed,
				    seed, offset, NFT_HASH_JENKINS);
	hash_expr->hash.expr = expr;
	return hash_expr;
}

static int fib_flag_parse(const char *name, int *flags)
{
	const char *fib_flags[] = {
		"saddr",
		"daddr",
		"mark",
		"iif",
		"oif",
	};
	unsigned int i;

	for (i = 0; i < array_size(fib_flags); i++) {
		if (!strcmp(name, fib_flags[i])) {
			*flags |= (1 << i);
			return 0;
		}
	}
	return 1;
}

static struct expr *json_parse_fib_expr(struct json_ctx *ctx,
					const char *type, json_t *root)
{
	const char *fib_result_tbl[] = {
		[NFT_FIB_RESULT_UNSPEC] = NULL,
		[NFT_FIB_RESULT_OIF] = "oif",
		[NFT_FIB_RESULT_OIFNAME] = "oifname",
		[NFT_FIB_RESULT_ADDRTYPE] = "type",
	};
	enum nft_fib_result resultval = NFT_FIB_RESULT_UNSPEC;
	json_t *flags, *value;
	const char *result;
	unsigned int i;
	size_t index;
	int flagval = 0;

	if (json_unpack_err(ctx, root, "{s:s}", "result", &result))
		return NULL;

	for (i = 1; i < array_size(fib_result_tbl); i++) {
		if (!strcmp(result, fib_result_tbl[i])) {
			resultval = i;
			break;
		}
	}
	if (resultval == NFT_FIB_RESULT_UNSPEC) {
		json_error(ctx, "Invalid fib result '%s'.", result);
		return NULL;
	}

	if (!json_unpack(root, "{s:o}", "flags", &flags)) {
		const char *flag;

		if (json_is_string(flags)) {
			flag = json_string_value(flags);

			if (fib_flag_parse(flag, &flagval)) {
				json_error(ctx, "Invalid fib flag '%s'.", flag);
				return NULL;
			}
		} else if (!json_is_array(flags)) {
			json_error(ctx, "Unexpected object type in fib tuple.");
			return NULL;
		}

		json_array_foreach(flags, index, value) {
			if (!json_is_string(value)) {
				json_error(ctx, "Unexpected object type in fib flags array at index %zd.", index);
				return NULL;
			}
			flag = json_string_value(value);

			if (fib_flag_parse(flag, &flagval)) {
				json_error(ctx, "Invalid fib flag '%s'.", flag);
				return NULL;
			}
		}
	}

	/* sanity checks from fib_expr in parser_bison.y */

	if ((flagval & (NFTA_FIB_F_SADDR|NFTA_FIB_F_DADDR)) == 0) {
		json_error(ctx, "fib: need either saddr or daddr");
		return NULL;
	}

	if ((flagval & (NFTA_FIB_F_SADDR|NFTA_FIB_F_DADDR)) ==
			(NFTA_FIB_F_SADDR|NFTA_FIB_F_DADDR)) {
		json_error(ctx, "fib: saddr and daddr are mutually exclusive");
		return NULL;
	}

	if ((flagval & (NFTA_FIB_F_IIF|NFTA_FIB_F_OIF)) ==
			(NFTA_FIB_F_IIF|NFTA_FIB_F_OIF)) {
		json_error(ctx, "fib: iif and oif are mutually exclusive");
		return NULL;
	}

	return fib_expr_alloc(int_loc, flagval, resultval);
}

static struct expr *json_parse_binop_expr(struct json_ctx *ctx,
					  const char *type, json_t *root)
{
	const struct {
		const char *type;
		enum ops op;
	} op_tbl[] = {
		{ "|", OP_OR },
		{ "^", OP_XOR },
		{ "&", OP_AND },
		{ ">>", OP_RSHIFT },
		{ "<<", OP_LSHIFT },
	};
	enum ops thisop = OP_INVALID;
	struct expr *left, *right;
	json_t *jleft, *jright;
	unsigned int i;

	for (i = 0; i < array_size(op_tbl); i++) {
		if (strcmp(type, op_tbl[i].type))
			continue;

		thisop = op_tbl[i].op;
		break;
	}
	if (thisop == OP_INVALID) {
		json_error(ctx, "Invalid binop type '%s'.", type);
		return NULL;
	}

	if (json_unpack_err(ctx, root, "[o, o!]", &jleft, &jright))
		return NULL;

	left = json_parse_primary_expr(ctx, jleft);
	if (!left) {
		json_error(ctx, "Failed to parse LHS of binop expression.");
		return NULL;
	}
	right = json_parse_primary_expr(ctx, jright);
	if (!right) {
		json_error(ctx, "Failed to parse RHS of binop expression.");
		expr_free(left);
		return NULL;
	}
	return binop_expr_alloc(int_loc, thisop, left, right);
}

static struct expr *json_parse_concat_expr(struct json_ctx *ctx,
					   const char *type, json_t *root)
{
	struct expr *expr = NULL, *tmp;
	json_t *value;
	size_t index;

	if (!json_is_array(root)) {
		json_error(ctx, "Unexpected concat object type %s.",
			   json_typename(root));
		return NULL;
	}

	json_array_foreach(root, index, value) {
		tmp = json_parse_primary_expr(ctx, value);
		if (!tmp) {
			json_error(ctx, "Parsing expr at index %zd failed.", index);
			expr_free(expr);
			return NULL;
		}
		if (!expr) {
			expr = tmp;
			continue;
		}
		if (expr->ops->type != EXPR_CONCAT) {
			struct expr *concat;

			concat = concat_expr_alloc(int_loc);
			compound_expr_add(concat, expr);
			expr = concat;
		}
		compound_expr_add(expr, tmp);
	}
	return expr;
}

static struct expr *json_parse_prefix_expr(struct json_ctx *ctx,
					   const char *type, json_t *root)
{
	struct expr *expr;
	json_t *addr;
	int len;

	if (json_unpack_err(ctx, root, "{s:o, s:i}",
			    "addr", &addr, "len", &len))
		return NULL;

	expr = json_parse_primary_expr(ctx, addr);
	if (!expr) {
		json_error(ctx, "Invalid prefix in prefix expr.");
		return NULL;
	}
	return prefix_expr_alloc(int_loc, expr, len);
}

static struct expr *json_parse_range_expr(struct json_ctx *ctx,
					  const char *type, json_t *root)
{
	struct expr *expr_low, *expr_high;
	json_t *low, *high;

	if (json_unpack_err(ctx, root, "[o, o!]", &low, &high))
		return NULL;

	expr_low = json_parse_primary_expr(ctx, low);
	if (!expr_low) {
		json_error(ctx, "Invalid low value in range expression.");
		return NULL;
	}
	expr_high = json_parse_primary_expr(ctx, high);
	if (!expr_high) {
		json_error(ctx, "Invalid high value in range expression.");
		return NULL;
	}
	return range_expr_alloc(int_loc, expr_low, expr_high);
}

static struct expr *json_parse_wildcard_expr(struct json_ctx *ctx,
					     const char *type, json_t *root)
{
	struct expr *expr;

	expr = constant_expr_alloc(int_loc, &integer_type,
				   BYTEORDER_HOST_ENDIAN, 0, NULL);
	return prefix_expr_alloc(int_loc, expr, 0);
}

static struct expr *json_parse_verdict_expr(struct json_ctx *ctx,
					    const char *type, json_t *root)
{
	const struct {
		int verdict;
		const char *name;
		bool chain;
	} verdict_tbl[] = {
		{ NFT_CONTINUE, "continue", false },
		{ NFT_BREAK, "break", false },
		{ NFT_JUMP, "jump", true },
		{ NFT_GOTO, "goto", true },
		{ NFT_RETURN, "return", false },
		{ NF_ACCEPT, "accept", false },
		{ NF_DROP, "drop", false },
		{ NF_QUEUE, "queue", false },
	};
	const char *chain = NULL;
	unsigned int i;

	json_unpack(root, "s", &chain);

	for (i = 0; i < array_size(verdict_tbl); i++) {
		if (strcmp(type, verdict_tbl[i].name))
			continue;

		if (verdict_tbl[i].chain && !chain) {
			json_error(ctx, "Verdict %s needs chain argument.", type);
			return NULL;
		}
		return verdict_expr_alloc(int_loc,
					  verdict_tbl[i].verdict, chain);
	}
	json_error(ctx, "Unknown verdict '%s'.", type);
	return NULL;
}

static struct expr *json_parse_set_expr(struct json_ctx *ctx,
					const char *type, json_t *root)
{
	struct expr *expr, *set_expr = NULL;
	json_t *value;
	size_t index;

	switch (json_typeof(root)) {
	case JSON_OBJECT:
	case JSON_ARRAY:
		break;
	default:
		expr = json_parse_immediate_expr(ctx, type, root);
		if (expr->ops->type == EXPR_SYMBOL &&
		    expr->symtype == SYMBOL_SET)
			return expr;

		expr = set_elem_expr_alloc(int_loc, expr);
		set_expr = set_expr_alloc(int_loc, NULL);
		compound_expr_add(set_expr, expr);
		return set_expr;
	}

	json_array_foreach(root, index, value) {
		struct expr *expr;
		json_t *jleft, *jright;

		if (!json_unpack(value, "[o, o!]", &jleft, &jright)) {
			struct expr *expr2;

			expr = json_parse_rhs_expr(ctx, jleft);
			if (!expr) {
				json_error(ctx, "Invalid set elem at index %zu.", index);
				expr_free(set_expr);
				return NULL;
			}
			if (expr->ops->type != EXPR_SET_ELEM)
				expr = set_elem_expr_alloc(int_loc, expr);

			expr2 = json_parse_set_rhs_expr(ctx, jright);
			if (!expr2) {
				json_error(ctx, "Invalid set elem at index %zu.", index);
				expr_free(expr);
				expr_free(set_expr);
				return NULL;
			}
			expr2 = mapping_expr_alloc(int_loc, expr, expr2);
			expr = expr2;
		} else if (json_is_object(value)) {
			expr = json_parse_rhs_expr(ctx, value);

			if (!expr) {
				json_error(ctx, "Invalid set elem at index %zu.", index);
				expr_free(set_expr);
				return NULL;
			}

			if (expr->ops->type != EXPR_SET_ELEM)
				expr = set_elem_expr_alloc(int_loc, expr);
		} else {
			expr = json_parse_immediate_expr(ctx, "elem", value);
			expr = set_elem_expr_alloc(int_loc, expr);
		}

		if (!set_expr)
			set_expr = set_expr_alloc(int_loc, NULL);
		compound_expr_add(set_expr, expr);
	}
	return set_expr;
}

static struct expr *json_parse_map_expr(struct json_ctx *ctx,
					const char *type, json_t *root)
{
	json_t *jleft, *jright;
	struct expr *left, *right;

	if (json_unpack_err(ctx, root, "{s:o, s:o}",
			    "left", &jleft, "right", &jright))
		return NULL;

	left = json_parse_map_lhs_expr(ctx, jleft);
	if (!left) {
		json_error(ctx, "Illegal LHS of map expression.");
		return NULL;
	}

	right = json_parse_rhs_expr(ctx, jright);
	if (!right) {
		json_error(ctx, "Illegal RHS of map expression.");
		expr_free(left);
		return NULL;
	}

	return map_expr_alloc(int_loc, left, right);
}

static struct expr *json_parse_set_elem_expr(struct json_ctx *ctx,
					     const char *type, json_t *root)
{
	struct expr *expr;
	json_t *tmp;
	int i;

	if (json_unpack_err(ctx, root, "{s:o}", "val", &tmp))
		return NULL;

	expr = json_parse_expr(ctx, tmp);
	if (!expr)
		return NULL;

	expr = set_elem_expr_alloc(int_loc, expr);

	if (!json_unpack(root, "{s:i}", "elem_timeout", &i))
		expr->timeout = i * 1000;
	if (!json_unpack(root, "{s:i}", "elem_expires", &i))
		expr->expiration = i * 1000;
	if (!json_unpack(root, "{s:s}", "elem_comment", &expr->comment))
		expr->comment = xstrdup(expr->comment);

	return expr;
}

static struct expr *json_parse_expr(struct json_ctx *ctx, json_t *root)
{
	const struct {
		const char *name;
		struct expr *(*cb)(struct json_ctx *, const char *, json_t *);
		uint32_t flags;
	} cb_tbl[] = {
		{ "concat", json_parse_concat_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_DTYPE | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "set", json_parse_set_expr, CTX_F_RHS | CTX_F_STMT }, /* allow this as stmt expr because that allows set references */
		{ "map", json_parse_map_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS },
		/* below three are multiton_rhs_expr */
		{ "prefix", json_parse_prefix_expr, CTX_F_RHS | CTX_F_STMT },
		{ "range", json_parse_range_expr, CTX_F_RHS | CTX_F_STMT },
		{ "*", json_parse_wildcard_expr, CTX_F_RHS | CTX_F_STMT },
		{ "immediate", json_parse_immediate_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP }, /* symbol, boolean or integer expr */
		{ "payload", json_parse_payload_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_MANGLE | CTX_F_SES | CTX_F_MAP },
		{ "exthdr", json_parse_exthdr_expr, CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "tcp option", json_parse_tcp_option_expr, CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_MANGLE | CTX_F_SES },
		{ "meta", json_parse_meta_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_MANGLE | CTX_F_SES | CTX_F_MAP },
		{ "rt", json_parse_rt_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "ct", json_parse_ct_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_MANGLE | CTX_F_SES | CTX_F_MAP },
		{ "numgen", json_parse_numgen_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		/* below two are hash expr */
		{ "jhash", json_parse_hash_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "symhash", json_parse_hash_expr, CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "fib", json_parse_fib_expr, CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "|", json_parse_binop_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "^", json_parse_binop_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "&", json_parse_binop_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ ">>", json_parse_binop_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "<<", json_parse_binop_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY | CTX_F_SET_RHS | CTX_F_SES | CTX_F_MAP },
		{ "accept", json_parse_verdict_expr, CTX_F_RHS | CTX_F_SET_RHS },
		{ "drop", json_parse_verdict_expr, CTX_F_RHS | CTX_F_SET_RHS },
		{ "continue", json_parse_verdict_expr, CTX_F_RHS | CTX_F_SET_RHS },
		{ "jump", json_parse_verdict_expr, CTX_F_RHS | CTX_F_SET_RHS },
		{ "goto", json_parse_verdict_expr, CTX_F_RHS | CTX_F_SET_RHS },
		{ "return", json_parse_verdict_expr, CTX_F_RHS | CTX_F_SET_RHS },
		{ "elem", json_parse_set_elem_expr, CTX_F_RHS | CTX_F_STMT | CTX_F_PRIMARY },
	};
	const char *type;
	unsigned int i;
	json_t *value;

	if (json_is_array(root)) {
		struct expr *list;
		size_t index;

		if (!(ctx->flags & (CTX_F_RHS | CTX_F_STMT))) {
			json_error(ctx, "List expression only allowed on RHS or in statement expression.");
			return NULL;
		}

		if (is_PRIMARY(ctx)) {
			json_error(ctx, "List expression not allowed as primary expression.");
			return NULL;
		}

		list = list_expr_alloc(int_loc);
		json_array_foreach(root, index, value) {
			struct expr *expr = json_parse_expr(ctx, value);
			if (!expr) {
				json_error(ctx, "Parsing list expression item at index %zu failed.", index);
				expr_free(list);
				return NULL;
			}
			compound_expr_add(list, expr);
		}
		return list;
	} else if (json_is_string(root)) {
		const struct datatype *dtype;

		if (is_DTYPE(ctx)) {
			dtype = datatype_lookup_byname(json_string_value(root));
			if (!dtype) {
				json_error(ctx, "Unknown datatype '%s'.", json_string_value(root));
				return NULL;
			}
			return constant_expr_alloc(int_loc, dtype,
						   dtype->byteorder, dtype->size, NULL);
		} else {
			return json_parse_immediate_expr(ctx, "immediate", root);
		}
	} else if ((is_RHS(ctx) || is_STMT(ctx) || is_PRIMARY(ctx)) && (json_is_integer(root) || json_is_boolean(root))) {
		/* is_STMT for mangle statement */
		return json_parse_immediate_expr(ctx, "immediate", root);
	}

	if (json_unpack_stmt(ctx, root, &type, &value))
		return NULL;

	for (i = 0; i < array_size(cb_tbl); i++) {
		if (strcmp(type, cb_tbl[i].name))
			continue;

		if ((cb_tbl[i].flags & ctx->flags) != ctx->flags) {
			json_error(ctx, "Expression type %s not allowed in context (%s).",
				   type, ctx_flags_to_string(ctx));
			return NULL;
		}

		return cb_tbl[i].cb(ctx, type, value);
	}
	json_error(ctx, "Unknown expression type '%s'.", type);
	return NULL;
}

static struct expr *json_parse_flagged_expr(struct json_ctx *ctx,
					    uint32_t flags, json_t *root)
{
	uint32_t old_flags = ctx->flags;
	struct expr *expr;

	ctx->flags |= flags;
	expr = json_parse_expr(ctx, root);
	ctx->flags = old_flags;

	return expr;
}

static struct expr *json_parse_rhs_expr(struct json_ctx *ctx, json_t *root)
{
	return json_parse_flagged_expr(ctx, CTX_F_RHS, root);
}

static struct expr *json_parse_stmt_expr(struct json_ctx *ctx, json_t *root)
{
	return json_parse_flagged_expr(ctx, CTX_F_STMT, root);
}

static struct expr *json_parse_primary_expr(struct json_ctx *ctx, json_t *root)
{
	return json_parse_flagged_expr(ctx, CTX_F_PRIMARY, root);
}

static struct expr *json_parse_set_rhs_expr(struct json_ctx *ctx, json_t *root)
{
	return json_parse_flagged_expr(ctx, CTX_F_SET_RHS, root);
}

static struct expr *json_parse_mangle_lhs_expr(struct json_ctx *ctx, json_t *root)
{
	return json_parse_flagged_expr(ctx, CTX_F_MANGLE, root);
}

static struct expr *json_parse_set_elem_expr_stmt(struct json_ctx *ctx, json_t *root)
{
	struct expr *expr = json_parse_flagged_expr(ctx, CTX_F_SES, root);

	if (expr->ops->type != EXPR_SET_ELEM)
		expr = set_elem_expr_alloc(int_loc, expr);

	return expr;
}

static struct expr *json_parse_map_lhs_expr(struct json_ctx *ctx, json_t *root)
{
	return json_parse_flagged_expr(ctx, CTX_F_MAP, root);
}

static struct expr *json_parse_dtype_expr(struct json_ctx *ctx, json_t *root)
{
	if (json_is_string(root)) {
		const struct datatype *dtype;

		dtype = datatype_lookup_byname(json_string_value(root));
		if (!dtype) {
			json_error(ctx, "Invalid datatype '%s'.",
				   json_string_value(root));
			return NULL;
		}
		return constant_expr_alloc(int_loc, dtype,
					   dtype->byteorder, dtype->size, NULL);
	} else if (json_is_array(root)) {
		json_t *value;
		size_t index;
		struct expr *expr = concat_expr_alloc(int_loc);

		json_array_foreach(root, index, value) {
			struct expr *i = json_parse_dtype_expr(ctx, value);

			if (!i) {
				json_error(ctx, "Invalid datatype at index %zu.", index);
				expr_free(expr);
				return NULL;
			}
			compound_expr_add(expr, i);
		}
		return expr;
	}
	json_error(ctx, "Invalid set datatype.");
	return NULL;
}

static struct stmt *json_parse_match_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	struct expr *left, *right, *rel_expr;
	json_t *jleft, *jright;
	const char *opstr = NULL;
	enum ops op;

	if (json_unpack_err(ctx, value, "{s:o, s:o}",
			    "left", &jleft,
			    "right", &jright))
		return NULL;

	json_unpack(value, "{s:s}", "op", &opstr);
	if (opstr) {
		for (op = OP_INVALID; op < __OP_MAX; op++) {
			if (expr_op_symbols[op] &&
			    !strcmp(opstr, expr_op_symbols[op]))
				break;
		}
		if (op == __OP_MAX) {
			json_error(ctx, "Unknown relational op '%s'.", opstr);
			return NULL;
		}
	} else {
		op = OP_IMPLICIT;
	}

	left = json_parse_expr(ctx, jleft);
	if (!left) {
		json_error(ctx, "Invalid LHS of relational.");
		return NULL;
	}
	right = json_parse_rhs_expr(ctx, jright);
	if (!right) {
		expr_free(left);
		json_error(ctx, "Invalid RHS of relational.");
		return NULL;
	}

	rel_expr = relational_expr_alloc(int_loc, op, left, right);
	return expr_stmt_alloc(int_loc, rel_expr);
}

static struct stmt *json_parse_counter_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	int packets, bytes;
	struct stmt *stmt;

	if (json_is_null(value))
		return counter_stmt_alloc(int_loc);

	if (!json_unpack(value, "{s:i, s:i}",
			    "packets", &packets,
			    "bytes", &bytes)) {
		stmt = counter_stmt_alloc(int_loc);
		stmt->counter.packets = packets;
		stmt->counter.bytes = bytes;
		return stmt;
	}

	stmt = objref_stmt_alloc(int_loc);
	stmt->objref.type = NFT_OBJECT_COUNTER;
	stmt->objref.expr = json_parse_stmt_expr(ctx, value);
	if (!stmt->objref.expr) {
		json_error(ctx, "Invalid counter reference.");
		stmt_free(stmt);
		return NULL;
	}
	return stmt;
}

static struct stmt *json_parse_verdict_stmt(struct json_ctx *ctx,
					    const char *key, json_t *value)
{
	struct {
		const char *name;
		int val;
	} verdict_type_tbl[] = {
		{ "accept", NF_ACCEPT },
		{ "drop", NF_DROP },
		{ "continue", NFT_CONTINUE },
		{ "jump", NFT_JUMP },
		{ "goto", NFT_GOTO },
		{ "return", NFT_RETURN },
	};
	const char *identifier = NULL;
	struct expr *expr;
	unsigned int i;
	int type = 255;	/* NFT_* are negative, NF_* are max 5 (NF_STOP) */

	for (i = 0; i < array_size(verdict_type_tbl); i++) {
		if (!strcmp(verdict_type_tbl[i].name, key)) {
			type = verdict_type_tbl[i].val;
			break;
		}
	}
	switch(type) {
	case NFT_JUMP:
	case NFT_GOTO:
		if (!json_is_string(value)) {
			json_error(ctx, "Verdict '%s' requires destination.", key);
			return NULL;
		}
		identifier = xstrdup(json_string_value(value));
		/* fall through */
	case NF_ACCEPT:
	case NF_DROP:
	case NFT_CONTINUE:
	case NFT_RETURN:
		expr = verdict_expr_alloc(int_loc, type, identifier);
		return verdict_stmt_alloc(int_loc, expr);
	}
	json_error(ctx, "Unknown verdict '%s'.", key);
	return NULL;
}

static struct stmt *json_parse_mangle_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	json_t *jleft, *jright;
	struct expr *left, *right;
	struct stmt *stmt;

	if (json_unpack_err(ctx, value, "{s:o, s:o}",
			   "left", &jleft, "right", &jright))
		return NULL;

	left = json_parse_mangle_lhs_expr(ctx, jleft);
	if (!left) {
		json_error(ctx, "Invalid LHS of mangle statement");
		return NULL;
	}
	right = json_parse_stmt_expr(ctx, jright);
	if (!right) {
		json_error(ctx, "Invalid RHS of mangle statement");
		expr_free(left);
		return NULL;
	}

	switch (left->ops->type) {
	case EXPR_EXTHDR:
		return exthdr_stmt_alloc(int_loc, left, right);
	case EXPR_PAYLOAD:
		return payload_stmt_alloc(int_loc, left, right);
	case EXPR_META:
		stmt = meta_stmt_alloc(int_loc, left->meta.key, right);
		expr_free(left);
		return stmt;
	case EXPR_CT:
		if (left->ct.key == NFT_CT_HELPER) {
			stmt = objref_stmt_alloc(int_loc);
			stmt->objref.type = NFT_OBJECT_CT_HELPER;
			stmt->objref.expr = right;
		} else {
			stmt = ct_stmt_alloc(int_loc, left->ct.key,
					     left->ct.direction, right);
		}
		expr_free(left);
		return stmt;
	default:
		json_error(ctx, "Invalid LHS expression type for mangle statement.");
		return NULL;
	}
}

static uint64_t rate_to_bytes(int val, const char *unit)
{
	uint64_t bytes = val;

	if (!strcmp(unit, "kbytes"))
		return bytes * 1024;
	if (!strcmp(unit, "mbytes"))
		return bytes * 1024 * 1024;
	return bytes;
}

static struct stmt *json_parse_quota_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	struct stmt *stmt;
	int inv = 0;
	const char *val_unit = "bytes", *used_unit = "bytes";
	int val, used = 0;

	if (!json_unpack(value, "{s:i}", "val", &val)) {
		json_unpack(value, "{s:b}", "inv", &inv);
		json_unpack(value, "{s:s}", "val_unit", &val_unit);
		json_unpack(value, "{s:i}", "used", &used);
		json_unpack(value, "{s:s}", "used_unit", &used_unit);
		stmt = quota_stmt_alloc(int_loc);
		stmt->quota.bytes = rate_to_bytes(val, val_unit);
		if (used)
			stmt->quota.used = rate_to_bytes(used, used_unit);
		stmt->quota.flags = (inv ? NFT_QUOTA_F_INV : 0);
		return stmt;
	}
	stmt = objref_stmt_alloc(int_loc);
	stmt->objref.type = NFT_OBJECT_QUOTA;
	stmt->objref.expr = json_parse_stmt_expr(ctx, value);
	if (!stmt->objref.expr) {
		json_error(ctx, "Invalid quota reference.");
		stmt_free(stmt);
		return NULL;
	}
	return stmt;
}

static uint64_t seconds_from_unit(const char *unit)
{
	if (!strcmp(unit, "week"))
		return 60 * 60 * 24 * 7;
	if (!strcmp(unit, "day"))
		return 60 * 60 * 24;
	if (!strcmp(unit, "hour"))
		return 60 * 60;
	if (!strcmp(unit, "minute"))
		return 60;
	return 1;
}

static struct stmt *json_parse_limit_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	struct stmt *stmt;
	int rate, burst = 0;
	const char *rate_unit = "packets", *time, *burst_unit = "bytes";
	int inv = 0;

	if (!json_unpack(value, "{s:i, s:s}",
			   "rate", &rate, "per", &time)) {
		json_unpack(value, "{s:s}", "rate_unit", &rate_unit);
		json_unpack(value, "{s:b}", "inv", &inv);
		json_unpack(value, "{s:i}", "burst", &burst);
		json_unpack(value, "{s:s}", "burst_unit", &burst_unit);

		stmt = limit_stmt_alloc(int_loc);

		if (!strcmp(rate_unit, "packets")) {
			stmt->limit.type = NFT_LIMIT_PKTS;
			stmt->limit.rate = rate;
			stmt->limit.burst = burst;
		} else {
			stmt->limit.type = NFT_LIMIT_PKT_BYTES;
			stmt->limit.rate = rate_to_bytes(rate, rate_unit);
			stmt->limit.burst = rate_to_bytes(burst, burst_unit);
		}
		stmt->limit.unit = seconds_from_unit(time);
		stmt->limit.flags = inv ? NFT_LIMIT_F_INV : 0;
		return stmt;
	}

	stmt = objref_stmt_alloc(int_loc);
	stmt->objref.type = NFT_OBJECT_LIMIT;
	stmt->objref.expr = json_parse_stmt_expr(ctx, value);
	if (!stmt->objref.expr) {
		json_error(ctx, "Invalid limit reference.");
		stmt_free(stmt);
		return NULL;
	}
	return stmt;
}

static struct stmt *json_parse_fwd_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	struct stmt *stmt = fwd_stmt_alloc(int_loc);

	stmt->fwd.to = json_parse_expr(ctx, value);

	return stmt;
}

static struct stmt *json_parse_notrack_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	return notrack_stmt_alloc(int_loc);
}

static struct stmt *json_parse_dup_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	struct stmt *stmt;
	struct expr *expr;
	json_t *tmp;

	if (json_unpack_err(ctx, value, "{s:o}", "addr", &tmp))
		return NULL;

	expr = json_parse_stmt_expr(ctx, tmp);
	if (!expr) {
		json_error(ctx, "Illegal dup addr arg.");
		return NULL;
	}

	stmt = dup_stmt_alloc(int_loc);
	stmt->dup.to = expr;

	if (json_unpack(value, "{s:o}", "dev", &tmp))
		return stmt;

	expr = json_parse_stmt_expr(ctx, tmp);
	if (!expr) {
		json_error(ctx, "Illegal dup dev.");
		stmt_free(stmt);
		return NULL;
	}
	stmt->dup.dev = expr;
	return stmt;
}

static int json_parse_nat_flag(struct json_ctx *ctx,
			       json_t *root, int *flags)
{
	const struct {
		const char *flag;
		int val;
	} flag_tbl[] = {
		{ "random", NF_NAT_RANGE_PROTO_RANDOM },
		{ "fully-random", NF_NAT_RANGE_PROTO_RANDOM_FULLY },
		{ "persistent", NF_NAT_RANGE_PERSISTENT },
	};
	const char *flag;
	unsigned int i;

	assert(flags);

	if (!json_is_string(root)) {
		json_error(ctx, "Invalid nat flag type %s, expected string.",
			   json_typename(root));
		return 1;
	}
	flag = json_string_value(root);
	for (i = 0; i < array_size(flag_tbl); i++) {
		if (!strcmp(flag, flag_tbl[i].flag)) {
			*flags |= flag_tbl[i].val;
			return 0;
		}
	}
	json_error(ctx, "Unknown nat flag '%s'.", flag);
	return 1;
}

static int json_parse_nat_flags(struct json_ctx *ctx, json_t *root)
{
	int flags = 0;
	json_t *value;
	size_t index;

	if (json_is_string(root)) {
		json_parse_nat_flag(ctx, root, &flags);
		return flags;
	} else if (!json_is_array(root)) {
		json_error(ctx, "Invalid nat flags type %s.",
			   json_typename(root));
		return -1;
	}
	json_array_foreach(root, index, value) {
		if (json_parse_nat_flag(ctx, value, &flags))
			json_error(ctx, "Parsing nat flag at index %zu failed.",
				   index);
	}
	return flags;
}

static int nat_type_parse(const char *type)
{
	const char * const nat_etypes[] = {
		[NFT_NAT_SNAT]	= "snat",
		[NFT_NAT_DNAT]	= "dnat",
		[NFT_NAT_MASQ]	= "masquerade",
		[NFT_NAT_REDIR]	= "redirect",
	};
	size_t i;

	for (i = 0; i < array_size(nat_etypes); i++) {
		if (!strcmp(type, nat_etypes[i]))
			return i;
	}
	return -1;
}

static struct stmt *json_parse_nat_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	struct stmt *stmt;
	json_t *tmp;
	int type;

	type = nat_type_parse(key);
	if (type < 0) {
		json_error(ctx, "Unknown nat type '%s'.", key);
		return NULL;
	}

	stmt = nat_stmt_alloc(int_loc, type);

	if (!json_unpack(value, "{s:o}", "addr", &tmp)) {
		stmt->nat.addr = json_parse_stmt_expr(ctx, tmp);
		if (!stmt->nat.addr) {
			json_error(ctx, "Invalid nat addr.");
			stmt_free(stmt);
			return NULL;
		}
	}
	if (!json_unpack(value, "{s:o}", "port", &tmp)) {
		stmt->nat.proto = json_parse_stmt_expr(ctx, tmp);
		if (!stmt->nat.proto) {
			json_error(ctx, "Invalid nat port.");
			stmt_free(stmt);
			return NULL;
		}
	}
	if (!json_unpack(value, "{s:o}", "flags", &tmp)) {
		int flags = json_parse_nat_flags(ctx, tmp);

		if (flags < 0) {
			stmt_free(stmt);
			return NULL;
		}
		stmt->nat.flags = flags;
	}
	return stmt;
}

static struct stmt *json_parse_reject_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	struct stmt *stmt = reject_stmt_alloc(int_loc);
	const struct datatype *dtype = NULL;
	const char *type;
	json_t *tmp;

	stmt->reject.type = -1;
	stmt->reject.icmp_code = -1;

	if (!json_unpack(value, "{s:s}", "type", &type)) {
		if (!strcmp(type, "tcp reset")) {
			stmt->reject.type = NFT_REJECT_TCP_RST;
			stmt->reject.icmp_code = 0;
		} else if (!strcmp(type, "icmpx")) {
			stmt->reject.type = NFT_REJECT_ICMPX_UNREACH;
			dtype = &icmpx_code_type;
			stmt->reject.icmp_code = 0;
		} else if (!strcmp(type, "icmp")) {
			stmt->reject.type = NFT_REJECT_ICMP_UNREACH;
			stmt->reject.family = NFPROTO_IPV4;
			dtype = &icmp_code_type;
			stmt->reject.icmp_code = 0;
		} else if (!strcmp(type, "icmpv6")) {
			stmt->reject.type = NFT_REJECT_ICMP_UNREACH;
			stmt->reject.family = NFPROTO_IPV6;
			dtype = &icmpv6_code_type;
			stmt->reject.icmp_code = 0;
		}
	}
	if (!json_unpack(value, "{s:o}", "expr", &tmp)) {
		stmt->reject.expr = json_parse_immediate_expr(ctx, "immediate", tmp);
		if (!stmt->reject.expr) {
			json_error(ctx, "Illegal reject expr.");
			stmt_free(stmt);
			return NULL;
		}
		if (dtype)
			stmt->reject.expr->dtype = dtype;
	}
	return stmt;
}

static struct stmt *json_parse_set_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	const char *opstr, *set;
	struct expr *expr, *expr2;
	struct stmt *stmt;
	json_t *elem;
	uint64_t tmp;
	int op;

	if (json_unpack_err(ctx, value, "{s:s, s:o, s:s}",
			    "op", &opstr, "elem", &elem, "set", &set))
		return NULL;

	if (!strcmp(opstr, "add")) {
		op = NFT_DYNSET_OP_ADD;
	} else if (!strcmp(opstr, "update")) {
		op = NFT_DYNSET_OP_UPDATE;
	} else {
		json_error(ctx, "Unknown set statement op '%s'.", opstr);
		return NULL;
	}

	expr = json_parse_set_elem_expr_stmt(ctx, elem);
	if (!expr) {
		json_error(ctx, "Illegal set statement element.");
		return NULL;
	}

	if (!json_unpack(elem, "{s:I}", "elem_timeout", &tmp))
		expr->timeout = tmp * 1000;
	if (!json_unpack(elem, "{s:I}", "elem_expires", &tmp))
		expr->expiration = tmp * 1000;
	json_unpack(elem, "{s:s}", "elem_comment", &expr->comment);

	if (set[0] != '@') {
		json_error(ctx, "Illegal set reference in set statement.");
		expr_free(expr);
		return NULL;
	}
	expr2 = symbol_expr_alloc(int_loc, SYMBOL_SET, NULL, set + 1);

	stmt = set_stmt_alloc(int_loc);
	stmt->set.op = op;
	stmt->set.key = expr;
	stmt->set.set = expr2;
	return stmt;
}

static int json_parse_log_flag(struct json_ctx *ctx,
			       json_t *root, int *flags)
{
	const struct {
		const char *flag;
		int val;
	} flag_tbl[] = {
		{ "tcp sequence", NF_LOG_TCPSEQ },
		{ "tcp options", NF_LOG_TCPOPT },
		{ "ip options", NF_LOG_IPOPT },
		{ "skuid", NF_LOG_UID },
		{ "ether", NF_LOG_MACDECODE },
		{ "all", NF_LOG_MASK },
	};
	const char *flag;
	unsigned int i;

	assert(flags);

	if (!json_is_string(root)) {
		json_error(ctx, "Invalid log flag type %s, expected string.",
			   json_typename(root));
		return 1;
	}
	flag = json_string_value(root);
	for (i = 0; i < array_size(flag_tbl); i++) {
		if (!strcmp(flag, flag_tbl[i].flag)) {
			*flags |= flag_tbl[i].val;
			return 0;
		}
	}
	json_error(ctx, "Unknown log flag '%s'.", flag);
	return 1;
}

static int json_parse_log_flags(struct json_ctx *ctx, json_t *root)
{
	int flags = 0;
	json_t *value;
	size_t index;

	if (json_is_string(root)) {
		json_parse_log_flag(ctx, root, &flags);
		return flags;
	} else if (!json_is_array(root)) {
		json_error(ctx, "Invalid log flags type %s.",
			   json_typename(root));
		return -1;
	}
	json_array_foreach(root, index, value) {
		if (json_parse_log_flag(ctx, value, &flags))
			json_error(ctx, "Parsing log flag at index %zu failed.",
				   index);
	}
	return flags;
}

static struct stmt *json_parse_log_stmt(struct json_ctx *ctx,
					const char *key, json_t *value)
{
	const char *tmpstr;
	struct stmt *stmt;
	json_t *jflags;
	int tmp;

	stmt = log_stmt_alloc(int_loc);

	if (!json_unpack(value, "{s:s}", "prefix", &tmpstr)) {
		stmt->log.prefix = xstrdup(tmpstr);
		stmt->log.flags |= STMT_LOG_PREFIX;
	}
	if (!json_unpack(value, "{s:i}", "group", &tmp)) {
		stmt->log.group = tmp;
		stmt->log.flags |= STMT_LOG_GROUP;
	}
	if (!json_unpack(value, "{s:i}", "snaplen", &tmp)) {
		stmt->log.snaplen = tmp;
		stmt->log.flags |= STMT_LOG_SNAPLEN;
	}
	if (!json_unpack(value, "{s:i}", "queue-threshold", &tmp)) {
		stmt->log.qthreshold = tmp;
		stmt->log.flags |= STMT_LOG_QTHRESHOLD;
	}
	if (!json_unpack(value, "{s:s}", "level", &tmpstr)) {
		int level = log_level_parse(tmpstr);

		if (level < 0) {
			json_error(ctx, "Invalid log level '%s'.", tmpstr);
			stmt_free(stmt);
			return NULL;
		}
		stmt->log.level = level;
		stmt->log.flags |= STMT_LOG_LEVEL;
	}
	if (!json_unpack(value, "{s:o}", "flags", &jflags)) {
		int flags = json_parse_log_flags(ctx, jflags);

		if (flags < 0) {
			stmt_free(stmt);
			return NULL;
		}
		stmt->log.logflags = flags;
	}
	return stmt;
}

static struct stmt *json_parse_cthelper_stmt(struct json_ctx *ctx,
					     const char *key, json_t *value)
{
	struct stmt *stmt = objref_stmt_alloc(int_loc);

	stmt->objref.type = NFT_OBJECT_CT_HELPER;
	stmt->objref.expr = json_parse_stmt_expr(ctx, value);
	if (!stmt->objref.expr) {
		json_error(ctx, "Invalid cthelper reference.");
		stmt_free(stmt);
		return NULL;
	}
	return stmt;
}

static struct stmt *json_parse_meter_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	json_t *jkey, *jstmt;
	struct stmt *stmt;
	const char *name;

	if (json_unpack_err(ctx, value, "{s:o, s:o}",
			    "key", &jkey, "stmt", &jstmt))
		return NULL;

	stmt = meter_stmt_alloc(int_loc);

	if (!json_unpack(value, "{s:s}", "name", &name))
		stmt->meter.name = xstrdup(name);

	stmt->meter.key = json_parse_expr(ctx, jkey);
	if (!stmt->meter.key) {
		json_error(ctx, "Invalid meter key.");
		stmt_free(stmt);
		return NULL;
	}

	stmt->meter.stmt = json_parse_stmt(ctx, jstmt);
	if (!stmt->meter.stmt) {
		json_error(ctx, "Invalid meter statement.");
		stmt_free(stmt);
		return NULL;
	}
	return stmt;
}

static int queue_flag_parse(const char *name, uint16_t *flags)
{
	if (!strcmp(name, "bypass"))
		*flags |= NFT_QUEUE_FLAG_BYPASS;
	else if (!strcmp(name, "fanout"))
		*flags |= NFT_QUEUE_FLAG_CPU_FANOUT;
	else
		return 1;
	return 0;
}

static struct stmt *json_parse_queue_stmt(struct json_ctx *ctx,
					  const char *key, json_t *value)
{
	struct stmt *stmt = queue_stmt_alloc(int_loc);
	json_t *tmp;

	if (!json_unpack(value, "{s:o}", "num", &tmp)) {
		stmt->queue.queue = json_parse_stmt_expr(ctx, tmp);
		if (!stmt->queue.queue) {
			json_error(ctx, "Invalid queue num.");
			stmt_free(stmt);
			return NULL;
		}
	}
	if (!json_unpack(value, "{s:o}", "flags", &tmp)) {
		const char *flag;
		size_t index;
		json_t *val;

		if (json_is_string(tmp)) {
			flag = json_string_value(tmp);

			if (queue_flag_parse(flag, &stmt->queue.flags)) {
				json_error(ctx, "Invalid queue flag '%s'.",
					   flag);
				stmt_free(stmt);
				return NULL;
			}
		} else if (!json_is_array(tmp)) {
			json_error(ctx, "Unexpected object type in queue flags.");
			stmt_free(stmt);
			return NULL;
		}

		json_array_foreach(tmp, index, val) {
			if (!json_is_string(val)) {
				json_error(ctx, "Invalid object in queue flag array at index %zu.",
					   index);
				stmt_free(stmt);
				return NULL;
			}
			flag = json_string_value(val);

			if (queue_flag_parse(flag, &stmt->queue.flags)) {
				json_error(ctx, "Invalid queue flag '%s'.",
					   flag);
				stmt_free(stmt);
				return NULL;
			}
		}
	}
	return stmt;
}

static struct stmt *json_parse_stmt(struct json_ctx *ctx, json_t *root)
{
	struct {
		const char *key;
		struct stmt *(*cb)(struct json_ctx *, const char *, json_t *);
	} stmt_parser_tbl[] = {
		{ "accept", json_parse_verdict_stmt },
		{ "drop", json_parse_verdict_stmt },
		{ "continue", json_parse_verdict_stmt },
		{ "jump", json_parse_verdict_stmt },
		{ "goto", json_parse_verdict_stmt },
		{ "return", json_parse_verdict_stmt },
		{ "match", json_parse_match_stmt },
		{ "counter", json_parse_counter_stmt },
		{ "mangle", json_parse_mangle_stmt },
		{ "quota", json_parse_quota_stmt },
		{ "limit", json_parse_limit_stmt },
		{ "fwd", json_parse_fwd_stmt },
		{ "notrack", json_parse_notrack_stmt },
		{ "dup", json_parse_dup_stmt },
		{ "snat", json_parse_nat_stmt },
		{ "dnat", json_parse_nat_stmt },
		{ "masquerade", json_parse_nat_stmt },
		{ "redirect", json_parse_nat_stmt },
		{ "reject", json_parse_reject_stmt },
		{ "set", json_parse_set_stmt },
		{ "log", json_parse_log_stmt },
		{ "ct helper", json_parse_cthelper_stmt },
		{ "meter", json_parse_meter_stmt },
		{ "queue", json_parse_queue_stmt },
	};
	const char *type;
	unsigned int i;
	json_t *tmp;

	if (json_unpack_stmt(ctx, root, &type, &tmp))
		return NULL;

	/* Yes, verdict_map_stmt is actually an expression */
	if (!strcmp(type, "map")) {
		struct expr *expr = json_parse_map_expr(ctx, type, tmp);

		if (!expr) {
			json_error(ctx, "Illegal vmap statement.");
			return NULL;
		}
		return verdict_stmt_alloc(int_loc, expr);
	}

	for (i = 0; i < array_size(stmt_parser_tbl); i++) {
		if (!strcmp(type, stmt_parser_tbl[i].key))
			return stmt_parser_tbl[i].cb(ctx, stmt_parser_tbl[i].key, tmp);
	}

	json_error(ctx, "Unknown statement object '%s'.", type);
	return NULL;
}

static struct cmd *json_parse_cmd_add_table(struct json_ctx *ctx, json_t *root,
					    enum cmd_ops op, enum cmd_obj obj)
{
	struct handle h = { 0 };
	const char *family = "";

	if (json_unpack_err(ctx, root, "{s:s}",
			    "family", &family))
		return NULL;
	if (op != CMD_DELETE &&
	    json_unpack_err(ctx, root, "{s:s}", "name", &h.table.name)) {
		return NULL;
	} else if (op == CMD_DELETE &&
		   json_unpack(root, "{s:s}", "name", &h.table.name) &&
		   json_unpack(root, "{s:I}", "handle", &h.handle.id)) {
		json_error(ctx, "Either name or handle required to delete a table.");
		return NULL;
	}
	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	if (h.table.name)
		h.table.name = xstrdup(h.table.name);

	return cmd_alloc(op, obj, &h, int_loc, NULL);
}

static int parse_policy(const char *policy)
{
	if (!strcmp(policy, "accept"))
		return NF_ACCEPT;
	if (!strcmp(policy, "drop"))
		return NF_DROP;
	return -1;
}

static struct cmd *json_parse_cmd_add_chain(struct json_ctx *ctx, json_t *root,
					    enum cmd_ops op, enum cmd_obj obj)
{
	struct handle h = { 0 };
	const char *family = "", *policy = "", *type, *hookstr;
	int prio;
	struct chain *chain;

	if (json_unpack_err(ctx, root, "{s:s, s:s}",
			    "family", &family,
			    "table", &h.table.name))
		return NULL;
	if (op != CMD_DELETE &&
	    json_unpack_err(ctx, root, "{s:s}", "name", &h.chain.name)) {
		return NULL;
	} else if (op == CMD_DELETE &&
		   json_unpack(root, "{s:s}", "name", &h.chain.name) &&
		   json_unpack(root, "{s:I}", "handle", &h.handle.id)) {
		json_error(ctx, "Either name or handle required to delete a chain.");
		return NULL;
	}
	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	if (h.chain.name)
		h.chain.name = xstrdup(h.chain.name);

	if (op == CMD_DELETE ||
	    op == CMD_LIST ||
	    op == CMD_FLUSH ||
	    json_unpack(root, "{s:s, s:s, s:i}",
			"type", &type, "hook", &hookstr, "prio", &prio))
		return cmd_alloc(op, obj, &h, int_loc, NULL);

	chain = chain_alloc(NULL);
	chain->flags |= CHAIN_F_BASECHAIN;
	chain->type = xstrdup(type);
	chain->hookstr = chain_hookname_lookup(hookstr);
	if (!chain->hookstr) {
		json_error(ctx, "Invalid chain hook '%s'.", hookstr);
		chain_free(chain);
		return NULL;
	}

	if (!json_unpack(root, "{s:s}", "dev", &chain->dev))
		chain->dev = xstrdup(chain->dev);
	if (!json_unpack(root, "{s:s}", "policy", &policy)) {
		chain->policy = parse_policy(policy);
		if (chain->policy < 0) {
			json_error(ctx, "Unknown policy '%s'.", policy);
			chain_free(chain);
			return NULL;
		}
	}

	handle_merge(&chain->handle, &h);
	return cmd_alloc(op, obj, &h, int_loc, chain);
}

static struct cmd *json_parse_cmd_add_rule(struct json_ctx *ctx, json_t *root,
					   enum cmd_ops op, enum cmd_obj obj)
{
	struct handle h = { 0 };
	const char *family = "", *comment = NULL;
	struct rule *rule;
	size_t index;
	json_t *tmp, *value;

	if (json_unpack_err(ctx, root, "{s:s, s:s, s:s}",
			    "family", &family,
			    "table", &h.table.name,
			    "chain", &h.chain.name))
		return NULL;
	if (op != CMD_DELETE &&
	    json_unpack_err(ctx, root, "{s:o}", "expr", &tmp))
		return NULL;
	else if (op == CMD_DELETE &&
		 json_unpack_err(ctx, root, "{s:I}", "handle", &h.handle.id))
		return NULL;

	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	h.chain.name = xstrdup(h.chain.name);

	if (op == CMD_DELETE)
		return cmd_alloc(op, obj, &h, int_loc, NULL);

	if (!json_is_array(tmp)) {
		json_error(ctx, "Value of property \"expr\" must be an array.");
		return NULL;
	}

	json_unpack(root, "{s:i}", "pos", &h.position.id);

	rule = rule_alloc(int_loc, NULL);

	json_unpack(root, "{s:s}", "comment", &comment);
	if (comment)
		rule->comment = strdup(comment);

	json_array_foreach(tmp, index, value) {
		struct stmt *stmt;

		if (!json_is_object(value)) {
			json_error(ctx, "Unexpected expr array element of type %s, expected object.",
				   json_typename(value));
			rule_free(rule);
			return NULL;
		}

		stmt = json_parse_stmt(ctx, value);

		if (!stmt) {
			json_error(ctx, "Parsing expr array at index %zd failed.", index);
			rule_free(rule);
			return NULL;
		}

		rule->num_stmts++;
		list_add_tail(&stmt->list, &rule->stmts);
	}

	return cmd_alloc(op, obj, &h, int_loc, rule);
}

static int string_to_nft_object(const char *str)
{
	const char *obj_tbl[] = {
		[NFT_OBJECT_COUNTER] = "counter",
		[NFT_OBJECT_QUOTA] = "quota",
		[NFT_OBJECT_CT_HELPER] = "ct helper",
		[NFT_OBJECT_LIMIT] = "limit",
	};
	unsigned int i;

	for (i = 1; i < array_size(obj_tbl); i++) {
		if (!strcmp(str, obj_tbl[i]))
			return i;
	}
	return 0;
}

static int string_to_set_flag(const char *str)
{
	const struct {
		enum nft_set_flags val;
		const char *name;
	} flag_tbl[] = {
		{ NFT_SET_CONSTANT, "constant" },
		{ NFT_SET_INTERVAL, "interval" },
		{ NFT_SET_TIMEOUT, "timeout" },
	};
	unsigned int i;

	for (i = 0; i < array_size(flag_tbl); i++) {
		if (!strcmp(str, flag_tbl[i].name))
			return flag_tbl[i].val;
	}
	return 0;
}

static struct cmd *json_parse_cmd_add_set(struct json_ctx *ctx, json_t *root,
					  enum cmd_ops op, enum cmd_obj obj)
{
	struct handle h = { 0 };
	const char *family = "", *policy, *dtype_ext = NULL;
	struct set *set;
	json_t *tmp;

	if (json_unpack_err(ctx, root, "{s:s, s:s}",
			    "family", &family,
			    "table", &h.table.name))
		return NULL;
	if (op != CMD_DELETE &&
	    json_unpack_err(ctx, root, "{s:s}", "name", &h.set.name)) {
		return NULL;
	} else if (op == CMD_DELETE &&
		   json_unpack(root, "{s:s}", "name", &h.set.name) &&
		   json_unpack(root, "{s:I}", "handle", &h.handle.id)) {
		json_error(ctx, "Either name or handle required to delete a set.");
		return NULL;
	}

	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	if (h.set.name)
		h.set.name = xstrdup(h.set.name);

	switch (op) {
	case CMD_DELETE:
	case CMD_LIST:
	case CMD_FLUSH:
		return cmd_alloc(op, obj, &h, int_loc, NULL);
	default:
		break;
	}

	set = set_alloc(NULL);

	if (json_unpack(root, "{s:o}", "type", &tmp)) {
		json_error(ctx, "Invalid set type.");
		set_free(set);
		handle_free(&h);
		return NULL;
	}
	set->key = json_parse_dtype_expr(ctx, tmp);
	if (!set->key) {
		json_error(ctx, "Invalid set type.");
		set_free(set);
		handle_free(&h);
		return NULL;
	}

	if (!json_unpack(root, "{s:s}", "map", &dtype_ext)) {
		set->objtype = string_to_nft_object(dtype_ext);
		if (set->objtype) {
			set->flags |= NFT_SET_OBJECT;
		} else if (datatype_lookup_byname(dtype_ext)) {
			set->datatype = datatype_lookup_byname(dtype_ext);
			set->flags |= NFT_SET_MAP;
		} else {
			json_error(ctx, "Invalid map type '%s'.", dtype_ext);
			set_free(set);
			handle_free(&h);
			return NULL;
		}
	}
	if (!json_unpack(root, "{s:s}", "policy", &policy)) {
		if (!strcmp(policy, "performance"))
			set->policy = NFT_SET_POL_PERFORMANCE;
		else if (!strcmp(policy, "memory")) {
			set->policy = NFT_SET_POL_MEMORY;
		} else {
			json_error(ctx, "Unknown set policy '%s'.", policy);
			set_free(set);
			handle_free(&h);
			return NULL;
		}
	}
	if (!json_unpack(root, "{s:o}", "flags", &tmp)) {
		json_t *value;
		size_t index;

		json_array_foreach(tmp, index, value) {
			int flag;

			if (!json_is_string(value) ||
			    !(flag = string_to_set_flag(json_string_value(value)))) {
				json_error(ctx, "Invalid set flag at index %zu.", index);
				set_free(set);
				handle_free(&h);
				return NULL;
			}
			set->flags |= flag;
		}
	}
	if (!json_unpack(root, "{s:o}", "elem", &tmp)) {
		set->init = json_parse_set_expr(ctx, "elem", tmp);
		if (!set->init) {
			json_error(ctx, "Invalid set elem expression.");
			set_free(set);
			handle_free(&h);
			return NULL;
		}
	}
	if (!json_unpack(root, "{s:i}", "timeout", &set->timeout))
		set->timeout *= 1000;
	if (!json_unpack(root, "{s:i}", "gc-interval", &set->gc_int))
		set->gc_int *= 1000;
	json_unpack(root, "{s:i}", "size", &set->desc.size);

	handle_merge(&set->handle, &h);
	return cmd_alloc(op, obj, &h, int_loc, set);
}

static struct cmd *json_parse_cmd_add_element(struct json_ctx *ctx,
					      json_t *root, enum cmd_ops op,
					      enum cmd_obj cmd_obj)
{
	struct handle h = { 0 };
	const char *family;
	struct expr *expr;
	json_t *tmp;

	if (json_unpack_err(ctx, root, "{s:s, s:s, s:s, s:o}",
			    "family", &family,
			    "table", &h.table.name,
			    "name", &h.set.name,
			    "elem", &tmp))
		return NULL;

	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	h.set.name = xstrdup(h.set.name);

	expr = json_parse_set_expr(ctx, "elem", tmp);
	if (!expr) {
		json_error(ctx, "Invalid set.");
		handle_free(&h);
		return NULL;
	}
	return cmd_alloc(op, cmd_obj, &h, int_loc, expr);
}

static struct expr *json_parse_flowtable_devs(struct json_ctx *ctx,
					      json_t *root)
{
	struct expr *tmp, *expr = compound_expr_alloc(int_loc, NULL);
	const char *dev;
	json_t *value;
	size_t index;

	if (!json_unpack(root, "s", &dev)) {
		tmp = symbol_expr_alloc(int_loc, SYMBOL_VALUE, NULL, dev);
		compound_expr_add(expr, tmp);
		return expr;
	}
	if (!json_is_array(root)) {
		expr_free(expr);
		return NULL;
	}

	json_array_foreach(root, index, value) {
		if (json_unpack(value, "s", &dev)) {
			json_error(ctx, "Invalid flowtable dev at index %zu.",
				   index);
			expr_free(expr);
			return NULL;
		}
		tmp = symbol_expr_alloc(int_loc, SYMBOL_VALUE, NULL, dev);
		compound_expr_add(expr, tmp);
	}
	return expr;
}

static struct cmd *json_parse_cmd_add_flowtable(struct json_ctx *ctx,
						json_t *root, enum cmd_ops op,
						enum cmd_obj cmd_obj)
{
	const char *family, *hook, *hookstr;
	struct flowtable *flowtable;
	struct handle h = { 0 };
	json_t *devs;
	int prio;

	if (json_unpack_err(ctx, root, "{s:s, s:s, s:s}",
			    "family", &family,
			    "table", &h.table.name,
			    "name", &h.flowtable))
		return NULL;

	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	h.flowtable = xstrdup(h.flowtable);

	if (op == CMD_DELETE)
		return cmd_alloc(op, cmd_obj, &h, int_loc, NULL);

	if (json_unpack_err(ctx, root, "{s:s, s:I, s:o}",
			    "hook", &hook,
			    "prio", &prio,
			    "dev", &devs)) {
		handle_free(&h);
		return NULL;
	}

	hookstr = chain_hookname_lookup(hook);
	if (!hookstr) {
		json_error(ctx, "Invalid flowtable hook '%s'.", hook);
		handle_free(&h);
		return NULL;
	}

	flowtable = flowtable_alloc(int_loc);
	flowtable->hookstr = hookstr;
	flowtable->priority = prio;

	flowtable->dev_expr = json_parse_flowtable_devs(ctx, devs);
	if (!flowtable->dev_expr) {
		json_error(ctx, "Invalid flowtable dev.");
		flowtable_free(flowtable);
		handle_free(&h);
		return NULL;
	}
	return cmd_alloc(op, cmd_obj, &h, int_loc, flowtable);
}

static struct cmd *json_parse_cmd_add_object(struct json_ctx *ctx,
					     json_t *root, enum cmd_ops op,
					     enum cmd_obj cmd_obj)
{
	const char *family, *tmp;
	struct handle h = { 0 };
	struct obj *obj;

	if (json_unpack_err(ctx, root, "{s:s, s:s}",
			    "family", &family,
			    "table", &h.table.name))
		return NULL;
	if ((op != CMD_DELETE ||
	     cmd_obj == NFT_OBJECT_CT_HELPER) &&
	    json_unpack_err(ctx, root, "{s:s}", "name", &h.obj.name)) {
		return NULL;
	} else if (op == CMD_DELETE &&
		   cmd_obj != NFT_OBJECT_CT_HELPER &&
		   json_unpack(root, "{s:s}", "name", &h.obj.name) &&
		   json_unpack(root, "{s:I}", "handle", &h.handle.id)) {
		json_error(ctx, "Either name or handle required to delete an object.");
		return NULL;
	}

	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	if (h.obj.name)
		h.obj.name = xstrdup(h.obj.name);

	if (op == CMD_DELETE || op == CMD_LIST) {
		if (cmd_obj == NFT_OBJECT_CT_HELPER)
			return cmd_alloc_obj_ct(op, NFT_OBJECT_CT_HELPER,
						&h, int_loc, obj_alloc(int_loc));
		return cmd_alloc(op, cmd_obj, &h, int_loc, NULL);
	}

	obj = obj_alloc(int_loc);

	switch (cmd_obj) {
	case CMD_OBJ_COUNTER:
		obj->type = NFT_OBJECT_COUNTER;
		json_unpack(root, "{s:i}", "packets", &obj->counter.packets);
		json_unpack(root, "{s:i}", "bytes", &obj->counter.bytes);
		break;
	case CMD_OBJ_QUOTA:
		obj->type = NFT_OBJECT_QUOTA;
		json_unpack(root, "{s:i}", "bytes", &obj->quota.bytes);
		json_unpack(root, "{s:i}", "used", &obj->quota.used);
		json_unpack(root, "{s:b}", "inv", &obj->quota.flags);
		if (obj->quota.flags)
			obj->quota.flags = NFT_QUOTA_F_INV;
		break;
	case NFT_OBJECT_CT_HELPER:
		cmd_obj = CMD_OBJ_CT_HELPER;
		obj->type = NFT_OBJECT_CT_HELPER;
		if (!json_unpack(root, "{s:s}", "type", &tmp)) {
			int ret;

			ret = snprintf(obj->ct_helper.name,
				       sizeof(obj->ct_helper.name), "%s", tmp);
			if (ret < 0 ||
			    ret >= (int)sizeof(obj->ct_helper.name)) {
				json_error(ctx, "Invalid CT helper type '%s', max length is %zu.",
					   tmp, sizeof(obj->ct_helper.name));
				obj_free(obj);
				return NULL;
			}
		}
		if (!json_unpack(root, "{s:s}", "protocol", &tmp)) {
			if (!strcmp(tmp, "tcp")) {
				obj->ct_helper.l4proto = IPPROTO_TCP;
			} else if (!strcmp(tmp, "udp")) {
				obj->ct_helper.l4proto = IPPROTO_UDP;
			} else {
				json_error(ctx, "Invalid ct helper protocol '%s'.", tmp);
				obj_free(obj);
				return NULL;
			}
		}
		if (!json_unpack(root, "{s:s}", "l3proto", &tmp)) {
			int family = parse_family(tmp);

			if (family < 0) {
				json_error(ctx, "Invalid ct helper l3proto '%s'.", tmp);
				obj_free(obj);
				return NULL;
			}
			obj->ct_helper.l3proto = family;
		} else {
			obj->ct_helper.l3proto = NFPROTO_IPV4;
		}
		break;
	case CMD_OBJ_LIMIT:
		obj->type = NFT_OBJECT_LIMIT;
		json_unpack(root, "{s:i}", "rate", &obj->limit.rate);
		if (!json_unpack(root, "{s:s}", "per", &tmp))
			obj->limit.unit = seconds_from_unit(tmp);
		json_unpack(root, "{s:i}", "burst", &obj->limit.burst);
		if (!json_unpack(root, "{s:s}", "unit", &tmp)) {
			if (!strcmp(tmp, "packets")) {
				obj->limit.type = NFT_LIMIT_PKTS;
			} else if (!strcmp(tmp, "bytes")) {
				obj->limit.type = NFT_LIMIT_PKT_BYTES;
			} else {
				json_error(ctx, "Invalid limit unit '%s'.", tmp);
				obj_free(obj);
				return NULL;
			}
		}
		json_unpack(root, "{s:b}", "inv", &obj->limit.flags);
		if (obj->limit.flags)
			obj->limit.flags = NFT_LIMIT_F_INV;
		break;
	default:
		BUG("Invalid CMD '%d'", cmd_obj);
	}

	return cmd_alloc(op, cmd_obj, &h, int_loc, obj);
}

static struct cmd *json_parse_cmd_add(struct json_ctx *ctx,
				      json_t *root, enum cmd_ops op)
{
	struct {
		const char *key;
		enum cmd_obj obj;
		struct cmd *(*cb)(struct json_ctx *, json_t *,
				  enum cmd_ops, enum cmd_obj);
	} cmd_obj_table[] = {
		{ "table", CMD_OBJ_TABLE, json_parse_cmd_add_table },
		{ "chain", CMD_OBJ_CHAIN, json_parse_cmd_add_chain },
		{ "rule", CMD_OBJ_RULE, json_parse_cmd_add_rule },
		{ "set", CMD_OBJ_SET, json_parse_cmd_add_set },
		{ "map", CMD_OBJ_SET, json_parse_cmd_add_set },
		{ "element", CMD_OBJ_SETELEM, json_parse_cmd_add_element },
		{ "flowtable", CMD_OBJ_FLOWTABLE, json_parse_cmd_add_flowtable },
		{ "counter", CMD_OBJ_COUNTER, json_parse_cmd_add_object },
		{ "quota", CMD_OBJ_QUOTA, json_parse_cmd_add_object },
		{ "ct helper", NFT_OBJECT_CT_HELPER, json_parse_cmd_add_object },
		{ "limit", CMD_OBJ_LIMIT, json_parse_cmd_add_object }
	};
	unsigned int i;
	json_t *tmp;

	if (!json_is_object(root)) {
		json_error(ctx, "Value of add command must be object (got %s instead).",
			   json_typename(root));
		return NULL;
	}

	for (i = 0; i < array_size(cmd_obj_table); i++) {
		tmp = json_object_get(root, cmd_obj_table[i].key);
		if (!tmp)
			continue;

		if (op == CMD_CREATE && cmd_obj_table[i].obj == CMD_OBJ_RULE) {
			json_error(ctx, "Create command not available for rules.");
			return NULL;
		}

		return cmd_obj_table[i].cb(ctx, tmp, op, cmd_obj_table[i].obj);
	}
	json_error(ctx, "Unknown object passed to add command.");
	return NULL;
}

static struct cmd *json_parse_cmd_replace(struct json_ctx *ctx,
					  json_t *root, enum cmd_ops op)
{
	struct handle h = { 0 };
	json_t *tmp, *value;
	const char *family;
	struct rule *rule;
	size_t index;

	if (json_unpack_err(ctx, root, "{s:o}", "rule", &rule))
		return NULL;

	if (json_unpack_err(ctx, root, "{s:s, s:s, s:s, s:o}",
			    "family", &family,
			    "table", &h.table.name,
			    "chain", &h.chain.name,
			    "expr", &tmp))
		return NULL;

	if (op == CMD_REPLACE &&
	    json_unpack_err(ctx, root, "{s:I}", "handle", &h.handle.id))
		return NULL;

	if (op == CMD_INSERT &&
	    json_unpack_err(ctx, root, "{s:i}", "pos", &h.position.id))
		return NULL;

	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}

	if (!json_is_array(tmp)) {
		json_error(ctx, "Value of property \"expr\" must be an array.");
		return NULL;
	}

	h.table.name = xstrdup(h.table.name);
	h.chain.name = xstrdup(h.chain.name);

	rule = rule_alloc(int_loc, NULL);

	if (!json_unpack(root, "{s:s}", "comment", &rule->comment))
		rule->comment = xstrdup(rule->comment);

	json_array_foreach(tmp, index, value) {
		struct stmt *stmt;

		if (!json_is_object(value)) {
			json_error(ctx, "Unexpected expr array element of type %s, expected object.",
				   json_typename(value));
			rule_free(rule);
			return NULL;
		}

		stmt = json_parse_stmt(ctx, value);

		if (!stmt) {
			json_error(ctx, "Parsing expr array at index %zd failed.",
				   index);
			rule_free(rule);
			return NULL;
		}

		rule->num_stmts++;
		list_add_tail(&stmt->list, &rule->stmts);
	}

	return cmd_alloc(op, CMD_OBJ_RULE, &h, int_loc, rule);
}

static struct cmd *json_parse_cmd_list_multiple(struct json_ctx *ctx,
						json_t *root, enum cmd_ops op,
						enum cmd_obj obj)
{
	struct handle h = {
		.family = NFPROTO_UNSPEC,
	};
	const char *tmp;

	if (!json_unpack(root, "{s:s}", "family", &tmp)) {
		h.family = parse_family(tmp);
		if (h.family < 0) {
			json_error(ctx, "Unknown family '%s'.", tmp);
			return NULL;
		}
	}
	switch (obj) {
	case CMD_OBJ_SETS:
	case CMD_OBJ_COUNTERS:
	case CMD_OBJ_CT_HELPERS:
		if (!json_unpack(root, "{s:s}", "table", &tmp))
			h.table.name = xstrdup(tmp);
		break;
	default:
		break;
	}
	if (obj == CMD_OBJ_CT_HELPERS && !h.table.name) {
		json_error(ctx, "Listing ct helpers requires table reference.");
		return NULL;
	}
	return cmd_alloc(op, obj, &h, int_loc, NULL);
}

static struct cmd *json_parse_cmd_list(struct json_ctx *ctx,
				       json_t *root, enum cmd_ops op)
{
	struct {
		const char *key;
		enum cmd_obj obj;
		struct cmd *(*cb)(struct json_ctx *, json_t *,
				  enum cmd_ops, enum cmd_obj);
	} cmd_obj_table[] = {
		{ "table", CMD_OBJ_TABLE, json_parse_cmd_add_table },
		{ "tables", CMD_OBJ_TABLE, json_parse_cmd_list_multiple },
		{ "chain", CMD_OBJ_CHAIN, json_parse_cmd_add_chain },
		{ "chains", CMD_OBJ_CHAINS, json_parse_cmd_list_multiple },
		{ "set", CMD_OBJ_SET, json_parse_cmd_add_set },
		{ "sets", CMD_OBJ_SETS, json_parse_cmd_list_multiple },
		{ "map", CMD_OBJ_MAP, json_parse_cmd_add_set },
		{ "maps", CMD_OBJ_MAPS, json_parse_cmd_add_set },
		{ "counter", CMD_OBJ_COUNTER, json_parse_cmd_add_object },
		{ "counters", CMD_OBJ_COUNTERS, json_parse_cmd_list_multiple },
		{ "quota", CMD_OBJ_QUOTA, json_parse_cmd_add_object },
		{ "quotas", CMD_OBJ_QUOTAS, json_parse_cmd_list_multiple },
		{ "ct helper", NFT_OBJECT_CT_HELPER, json_parse_cmd_add_object },
		{ "ct helpers", CMD_OBJ_CT_HELPERS, json_parse_cmd_list_multiple },
		{ "limit", CMD_OBJ_LIMIT, json_parse_cmd_add_object },
		{ "limits", CMD_OBJ_LIMIT, json_parse_cmd_list_multiple },
		{ "ruleset", CMD_OBJ_RULESET, json_parse_cmd_list_multiple },
		{ "meter", CMD_OBJ_METER, json_parse_cmd_add_set },
		{ "meters", CMD_OBJ_METERS, json_parse_cmd_list_multiple },
		{ "flowtables", CMD_OBJ_FLOWTABLES, json_parse_cmd_list_multiple },
	};
	unsigned int i;
	json_t *tmp;

	if (!json_is_object(root)) {
		json_error(ctx, "Value of list command must be object (got %s instead).",
			   json_typename(root));
		return NULL;
	}

	for (i = 0; i < array_size(cmd_obj_table); i++) {
		tmp = json_object_get(root, cmd_obj_table[i].key);
		if (!tmp)
			continue;

		return cmd_obj_table[i].cb(ctx, tmp, op, cmd_obj_table[i].obj);
	}
	json_error(ctx, "Unknown object passed to list command.");
	return NULL;
}

static struct cmd *json_parse_cmd_reset(struct json_ctx *ctx,
				        json_t *root, enum cmd_ops op)
{
	struct {
		const char *key;
		enum cmd_obj obj;
		struct cmd *(*cb)(struct json_ctx *, json_t *,
				  enum cmd_ops, enum cmd_obj);
	} cmd_obj_table[] = {
		{ "counter", CMD_OBJ_COUNTER, json_parse_cmd_add_object },
		{ "counters", CMD_OBJ_COUNTERS, json_parse_cmd_list_multiple },
		{ "quota", CMD_OBJ_QUOTA, json_parse_cmd_add_object },
		{ "quotas", CMD_OBJ_QUOTAS, json_parse_cmd_list_multiple },
	};
	unsigned int i;
	json_t *tmp;

	if (!json_is_object(root)) {
		json_error(ctx, "Value of reset command must be object (got %s instead).",
			   json_typename(root));
		return NULL;
	}

	for (i = 0; i < array_size(cmd_obj_table); i++) {
		tmp = json_object_get(root, cmd_obj_table[i].key);
		if (!tmp)
			continue;

		return cmd_obj_table[i].cb(ctx, tmp, op, cmd_obj_table[i].obj);
	}
	json_error(ctx, "Unknown object passed to reset command.");
	return NULL;
}

static struct cmd *json_parse_cmd_flush(struct json_ctx *ctx,
				        json_t *root, enum cmd_ops op)
{
	struct {
		const char *key;
		enum cmd_obj obj;
		struct cmd *(*cb)(struct json_ctx *, json_t *,
				  enum cmd_ops, enum cmd_obj);
	} cmd_obj_table[] = {
		{ "table", CMD_OBJ_TABLE, json_parse_cmd_add_table },
		{ "chain", CMD_OBJ_CHAIN, json_parse_cmd_add_chain },
		{ "set", CMD_OBJ_SET, json_parse_cmd_add_set },
		{ "map", CMD_OBJ_MAP, json_parse_cmd_add_set },
		{ "meter", CMD_OBJ_METER, json_parse_cmd_add_set },
		{ "ruleset", CMD_OBJ_RULESET, json_parse_cmd_list_multiple },
	};
	unsigned int i;
	json_t *tmp;

	if (!json_is_object(root)) {
		json_error(ctx, "Value of flush command must be object (got %s instead).",
			   json_typename(root));
		return NULL;
	}

	for (i = 0; i < array_size(cmd_obj_table); i++) {
		tmp = json_object_get(root, cmd_obj_table[i].key);
		if (!tmp)
			continue;

		return cmd_obj_table[i].cb(ctx, tmp, op, cmd_obj_table[i].obj);
	}
	json_error(ctx, "Unknown object passed to flush command.");
	return NULL;
}

static struct cmd *json_parse_cmd_rename(struct json_ctx *ctx,
				         json_t *root, enum cmd_ops op)
{
	const char *family, *newname;
	struct handle h = { 0 };
	struct cmd *cmd;

	if (json_unpack_err(ctx, root, "{s:{s:s, s:s, s:s, s:s}}", "chain",
			    "family", &family,
			    "table", &h.table.name,
			    "name", &h.chain.name,
			    "newname", &newname))
		return NULL;
	h.family = parse_family(family);
	if (h.family < 0) {
		json_error(ctx, "Unknown family '%s'.", family);
		return NULL;
	}
	h.table.name = xstrdup(h.table.name);
	h.chain.name = xstrdup(h.chain.name);

	cmd = cmd_alloc(op, CMD_OBJ_CHAIN, &h, int_loc, NULL);
	cmd->arg = xstrdup(newname);
	return cmd;
}

static struct cmd *json_parse_cmd(struct json_ctx *ctx, json_t *root)
{
	struct {
		const char *key;
		enum cmd_ops op;
		struct cmd *(*cb)(struct json_ctx *ctx, json_t *, enum cmd_ops);
	} parse_cb_table[] = {
		{ "add", CMD_ADD, json_parse_cmd_add },
		{ "replace", CMD_REPLACE, json_parse_cmd_replace },
		{ "create", CMD_CREATE, json_parse_cmd_add },
		{ "insert", CMD_INSERT, json_parse_cmd_replace },
		{ "delete", CMD_DELETE, json_parse_cmd_add },
		{ "list", CMD_LIST, json_parse_cmd_list },
		{ "reset", CMD_RESET, json_parse_cmd_reset },
		{ "flush", CMD_FLUSH, json_parse_cmd_flush },
		{ "rename", CMD_RENAME, json_parse_cmd_rename },
		//{ "export", CMD_EXPORT, json_parse_cmd_export },
		//{ "monitor", CMD_MONITOR, json_parse_cmd_monitor },
		//{ "describe", CMD_DESCRIBE, json_parse_cmd_describe }
	};
	unsigned int i;
	json_t *tmp;

	for (i = 0; i < array_size(parse_cb_table); i++) {
		tmp = json_object_get(root, parse_cb_table[i].key);
		if (!tmp)
			continue;

		return parse_cb_table[i].cb(ctx, tmp, parse_cb_table[i].op);
	}
	json_error(ctx, "Unknown command object.");
	return NULL;
}

static int __json_parse(struct json_ctx *ctx, json_t *root)
{
	struct eval_ctx ectx = {
		.nf_sock = ctx->nft->nf_sock,
		.msgs = ctx->msgs,
		.cache = &ctx->nft->cache,
		.octx = &ctx->nft->output,
		.debug_mask = ctx->nft->debug_mask,
	};
	json_t *tmp, *value;
	size_t index;

	if (json_unpack_err(ctx, root, "{s:o}", "nftables", &tmp))
		return -1;

	if (!json_is_array(tmp)) {
		json_error(ctx, "Value of property \"nftables\" must be an array.");
		return -1;
	}

	json_array_foreach(tmp, index, value) {
		/* this is more or less from parser_bison.y:716 */
		LIST_HEAD(list);
		struct cmd *cmd;

		if (!json_is_object(value)) {
			json_error(ctx, "Unexpected command array element of type %s, expected object.", json_typename(value));
			return -1;
		}
		cmd = json_parse_cmd(ctx, value);

		if (!cmd) {
			json_error(ctx, "Parsing command array at index %zd failed.", index);
			return -1;
		}

		list_add_tail(&cmd->list, &list);

		if (cmd_evaluate(&ectx, cmd) < 0) {
			cmd_free(cmd);
			json_error(ctx, "Evaluating command at index %zd failed.", index);
			return -1;
		}
		list_splice_tail(&list, ctx->cmds);
	}

	return 0;
}


int nft_parse_json_buffer(struct nft_ctx *nft, char *buf, size_t buflen,
			  struct list_head *msgs, struct list_head *cmds)
{
	struct json_ctx ctx = {
		.indesc = {
			.type = INDESC_BUFFER,
			.data = buf,
		},
		.nft = nft,
		.msgs = msgs,
		.cmds = cmds,
	};
	json_t *root;
	int ret;

	root = json_loads(buf, 0, NULL);
	if (!root)
		return -EINVAL;

	ret = __json_parse(&ctx, root);

	json_decref(root);
	return ret;
}

int nft_parse_json_filename(struct nft_ctx *nft, const char *filename,
			    struct list_head *msgs, struct list_head *cmds)
{
	struct json_ctx ctx = {
		.indesc = {
			.type = INDESC_FILE,
			.name = filename,
		},
		.nft = nft,
		.msgs = msgs,
		.cmds = cmds,
	};
	json_error_t err;
	json_t *root;
	int ret;

	root = json_load_file(filename, 0, &err);
	if (!root)
		return -EINVAL;

	ret = __json_parse(&ctx, root);

	json_decref(root);
	return ret;
}
