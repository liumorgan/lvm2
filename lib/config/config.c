/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include "config.h"
#include "pool.h"
#include "log.h"

enum {
        TOK_INT,
        TOK_FLOAT,
        TOK_STRING,
        TOK_EQ,
        TOK_SECTION_B,
        TOK_SECTION_E,
        TOK_ARRAY_B,
        TOK_ARRAY_E,
        TOK_IDENTIFIER,
	TOK_COMMA,
	TOK_EOF
};

struct parser {
        const char *fb, *fe;    /* file limits */

        int t;                  /* token limits and type */
        const char *tb, *te;

        int fd;                 /* descriptor for file being parsed */
	int line;		/* line number we are on */

        struct pool *mem;
};

struct cs {
        struct config_file cf;
        struct pool *mem;
};

static void _get_token(struct parser *p);
static void _eat_space(struct parser *p);
static struct config_node *_file(struct parser *p);
static struct config_node *_section(struct parser *p);
static struct config_value *_value(struct parser *p);
static struct config_value *_type(struct parser *p);
static int _match_aux(struct parser *p, int t);
static struct config_value *_create_value(struct parser *p);
static struct config_node *_create_node(struct parser *p);
static char *_dup_tok(struct parser *p);
static int _tok_match(const char *str, const char *b, const char *e);

#define MAX_INDENT 32

#define match(t) do {\
   if (!_match_aux(p, (t))) {\
	log_error("Parse error at line %d: unexpected token", p->line); \
      return 0;\
   } \
} while(0);

/*
 * public interface
 */
struct config_file *create_config_file()
{
        struct cs *c;
        struct pool *mem = pool_create(10 * 1024);

        if (!mem) {
                stack;
                return 0;
        }

        if (!(c = pool_alloc(mem, sizeof(*c)))) {
                stack;
                pool_destroy(mem);
                return 0;
        }

        c->mem = mem;
	c->cf.root = (struct config_node *)NULL;
        return &c->cf;
}

void destroy_config_file(struct config_file *cf)
{
        pool_destroy(((struct cs *) cf)->mem);
}

int read_config(struct config_file *cf, const char *file)
{
	struct cs *c = (struct cs *) cf;
        struct parser *p;
        struct stat info;
        int r = 1, fd;

        if (!(p = pool_alloc(c->mem, sizeof(*p)))) {
                stack;
                return 0;
        }
	p->mem = c->mem;

        /* memory map the file */
        if (stat(file, &info) || S_ISDIR(info.st_mode)) {
                log_sys_error("stat", file);
                return 0;
        }

        if ((fd = open(file, O_RDONLY)) < 0) {
                log_sys_error("open", file);
                return 0;
        }

        p->fb = mmap((caddr_t) 0, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p->fb == MAP_FAILED) {
                log_sys_error("mmap", file);
                close(fd);
                return 0;
        }
        p->fe = p->fb + info.st_size;

        /* parse */
        p->tb = p->te = p->fb;
	p->line = 1;
	_get_token(p);
        if (!(cf->root = _file(p))) {
                stack;
                r = 0;
        }

        /* unmap the file */
        if (munmap((char *) p->fb, info.st_size)) {
                log_sys_error("munmap", file);
                r = 0;
        }

        close(fd);
        return r;
}

static void _write_value(FILE *fp, struct config_value *v)
{
	switch (v->type) {
	case CFG_STRING:
		fprintf(fp, "\"%s\"", v->v.str);
		break;

	case CFG_FLOAT:
		fprintf(fp, "%f", v->v.r);
		break;

	case CFG_INT:
		fprintf(fp, "%d", v->v.i);
		break;
	}
}

static int _write_config(struct config_node *n, FILE *fp, int level)
{
        char space[MAX_INDENT + 1];
        int l = (level < MAX_INDENT) ? level : MAX_INDENT;
        int i;

	if (!n)
		return 1;

        for (i = 0; i < l; i++)
                space[i] = ' ';
        space[i] = '\0';

        while (n) {
                fprintf(fp, "%s%s", space, n->key);
		if (!n->v) {
			/* it's a sub section */
			fprintf(fp, " {\n");
			_write_config(n->child, fp, level + 1);
			fprintf(fp, "%s}", space);
		} else {
			/* it's a value */
			struct config_value *v = n->v;
			fprintf(fp, "=");
			if (v->next) {
				fprintf(fp, "[");
				while (v) {
					_write_value(fp, v);
					v = v->next;
					if (v)
						fprintf(fp, ", ");
				}
				fprintf(fp, "]");
			} else
				_write_value(fp, v);
		}
		fprintf(fp, "\n");
                n = n->sib;
        }
	/* FIXME: add error checking */
	return 1;
}

int write_config(struct config_file *cf, const char *file)
{
	int r = 1;
        FILE *fp = fopen(file, "w");
        if (!fp) {
                log_sys_error("open", file);
                return 0;
        }

        if (!_write_config(cf->root, fp, 0)) {
		stack;
		r = 0;
	}
        fclose(fp);
	return r;
}

/*
 * parser
 */
static struct config_node *_file(struct parser *p)
{
	struct config_node *root = NULL, *n, *l = NULL;
	while (p->t != TOK_EOF) {
		if (!(n = _section(p))) {
			stack;
			return 0;
		}

		if (!root)
			root = n;
		else
			l->sib = n;
		l = n;
	}
	return root;
}

static struct config_node *_section(struct parser *p)
{
        /* IDENTIFIER '{' VALUE* '}' */
	struct config_node *root, *n, *l = NULL;
	if (!(root = _create_node(p))) {
		stack;
		return 0;
	}

	if (!(root->key = _dup_tok(p))) {
		stack;
		return 0;
	}

        match (TOK_IDENTIFIER);

	if (p->t == TOK_SECTION_B) {
		match(TOK_SECTION_B);
		while (p->t != TOK_SECTION_E) {
			if (!(n = _section(p))) {
				stack;
				return 0;
			}

			if (!root->child)
				root->child = n;
			else
				l->sib = n;
			l = n;
		}
		match(TOK_SECTION_E);
	} else {
		match(TOK_EQ);
		if (!(root->v = _value(p))) {
			stack;
			return 0;
		}
	}

        return root;
}

static struct config_value *_value(struct parser *p)
{
        /* '[' TYPE* ']' | TYPE */
	struct config_value *h = 0, *l, *ll = 0;
        if (p->t == TOK_ARRAY_B) {
                match (TOK_ARRAY_B);
                while (p->t != TOK_ARRAY_E) {
                        if (!(l = _type(p))) {
				stack;
				return 0;
			}

			if (!h)
				h = l;
			else
				ll->next = l;
			ll = l;

			if (p->t == TOK_COMMA)
				match(TOK_COMMA);
		}
                match(TOK_ARRAY_E);
        } else
		h = _type(p);

        return h;
}

static struct config_value *_type(struct parser *p)
{
        /* [0-9]+ | [0-9]*\.[0-9]* | ".*" */
	struct config_value *v = _create_value(p);

        switch (p->t) {
        case TOK_INT:
		v->type = CFG_INT;
		v->v.i = strtol(p->tb, 0, 10); /* FIXME: check error */
                match(TOK_INT);
                break;

        case TOK_FLOAT:
		v->type = CFG_FLOAT;
		v->v.r = strtod(p->tb, 0); /* FIXME: check error */
                match(TOK_FLOAT);
                break;

        case TOK_STRING:
		v->type = CFG_STRING;

		p->tb++, p->te--; /* strip "'s */
		if (!(v->v.str = _dup_tok(p))) {
			stack;
			return 0;
		}
		p->te++;
                match(TOK_STRING);
                break;

        default:
		log_error("Parse error at line %d: expected a value", p->line);
                return 0;
        }
        return v;
}

static int _match_aux(struct parser *p, int t)
{
	if (p->t != t)
		return 0;

	_get_token(p);
	return 1;
}

/*
 * tokeniser
 */
static void _get_token(struct parser *p)
{
        p->tb = p->te;
        _eat_space(p);
        if (p->tb == p->fe) {
		p->t = TOK_EOF;
		return;
	}

        p->t = TOK_INT;         /* fudge so the fall through for
                                   floats works */
        switch (*p->te) {
        case '{':
                p->t = TOK_SECTION_B;
                p->te++;
                break;

        case '}':
                p->t = TOK_SECTION_E;
                p->te++;
                break;

        case '[':
                p->t = TOK_ARRAY_B;
                p->te++;
                break;

        case ']':
                p->t = TOK_ARRAY_E;
                p->te++;
                break;

        case ',':
                p->t = TOK_COMMA;
                p->te++;
                break;

        case '=':
                p->t = TOK_EQ;
                p->te++;
                break;

        case '"':
                p->t = TOK_STRING;
		p->te++;
                while ((p->te != p->fe) && (*p->te != '"')) {
                        if ((*p->te == '\\') && (p->te + 1 != p->fe))
                                p->te++;
                        p->te++;
                }

                if (p->te != p->fe)
                        p->te++;
                break;

        case '.':
                p->t = TOK_FLOAT;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
                p->te++;
                while (p->te != p->fe) {
                        if (*p->te == '.') {
                                if (p->t == TOK_FLOAT)
                                        break;
                                p->t = TOK_FLOAT;
                        } else if (!isdigit((int) *p->te))
                                break;
                        p->te++;
                }
                break;

        default:
                p->t = TOK_IDENTIFIER;
                while ((p->te != p->fe) && !isspace(*p->te) &&
		      (*p->te != '#') && (*p->te != '='))
                        p->te++;
                break;
        }
}

static void _eat_space(struct parser *p)
{
        while (p->tb != p->fe) {
                if (*p->te == '#') {
                        while ((p->te != p->fe) && (*p->te != '\n'))
                                p->te++;
			p->line++;
		}

		else if (isspace(*p->te)) {
                        while ((p->te != p->fe) && isspace(*p->te)) {
				if (*p->te == '\n')
					p->line++;
                                p->te++;
			}
		}

                else
                        return;

                p->tb = p->te;
        }
}

/*
 * memory management
 */
static struct config_value *_create_value(struct parser *p)
{
	struct config_value *v = pool_alloc(p->mem, sizeof(*v));
	memset(v, 0, sizeof(*v));
	return v;
}

static struct config_node *_create_node(struct parser *p)
{
	struct config_node *n = pool_alloc(p->mem, sizeof(*n));
	memset(n, 0, sizeof(*n));
	return n;
}

static char *_dup_tok(struct parser *p)
{
	int len = p->te - p->tb;
	char *str = pool_alloc(p->mem, len + 1);
	if (!str) {
		stack;
		return 0;
	}
	strncpy(str, p->tb, len);
	str[len] = '\0';
	return str;
}

/*
 * utility functions
 */
struct config_node *find_config_node(struct config_node *cn,
				     const char *path, char sep)
{
	const char *e;

	while (cn) {
		/* trim any leading slashes */
		while (*path && (*path == sep))
			path++;

		/* find the end of this segment */
		for (e = path; *e && (*e != sep); e++)
			;

		/* hunt for the node */
		while (cn) {
			if (_tok_match(cn->key, path, e))
				break;

			cn = cn->sib;
		}

		if (cn && *e)
			cn = cn->child;
		else
			break;	/* don't move into the last node */

		path = e;
	}

	return cn;
}

const char *
find_config_str(struct config_node *cn,
		const char *path, char sep, const char *fail)
{
	struct config_node *n = find_config_node(cn, path, sep);

	if (n && n->v->type == CFG_STRING)
		return n->v->v.str;

	if (fail)
		log_debug("%s not found in config: defaulting to %s",
			  path, fail);
	return fail;
}

int find_config_int(struct config_node *cn, const char *path,
		    char sep, int fail)
{
	struct config_node *n = find_config_node(cn, path, sep);

	if (n && n->v->type == CFG_INT)
		return n->v->v.i;

	return fail;
}

float find_config_float(struct config_node *cn, const char *path,
			char sep, float fail)
{
	struct config_node *n = find_config_node(cn, path, sep);

	if (n && n->v->type == CFG_FLOAT)
		return n->v->v.r;

	return fail;

}

static int _tok_match(const char *str, const char *b, const char *e)
{
	while (*str && (b != e)) {
		if (*str++ != *b++)
			return 0;
	}

	return !(*str || (b != e));
}

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
