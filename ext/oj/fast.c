/* fast.c
 * Copyright (c) 2012, Peter Ohler
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 *  - Neither the name of Peter Ohler nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "oj.h"

// maximum to allocate on the stack, arbitrary limit
#define SMALL_XML	65536
#define MAX_STACK	100
//#define BATCH_SIZE	(4096 / sizeof(struct _Leaf) - 1)
#define BATCH_SIZE	100

typedef struct _Batch {
    struct _Batch	*next;
    int			next_avail;
    struct _Leaf	leaves[BATCH_SIZE];
} *Batch;

typedef struct _Doc {
    Leaf		data;
    Leaf		*where;	     // points to current location
    Leaf		where_path[MAX_STACK]; // points to head of path
    char		*json;
    unsigned long	size;	     // number of leaves/branches in the doc
    VALUE		self;
    Batch		batches;
    struct _Batch	batch0;
} *Doc;

typedef struct _ParseInfo {
    char	*str;		/* buffer being read from */
    char	*s;		/* current position in buffer */
    Doc		doc;
} *ParseInfo;

static void	leaf_init(Leaf leaf, int type);
static Leaf	leaf_new(Doc doc, int type);
static void	leaf_append_element(Leaf parent, Leaf element);
static VALUE	leaf_value(Doc doc, Leaf leaf);
static void	leaf_fixnum_value(Leaf leaf);
static void	leaf_float_value(Leaf leaf);
static VALUE	leaf_array_value(Doc doc, Leaf leaf);
static VALUE	leaf_hash_value(Doc doc, Leaf leaf);

static Leaf	read_next(ParseInfo pi);
static Leaf	read_obj(ParseInfo pi);
static Leaf	read_array(ParseInfo pi);
static Leaf	read_str(ParseInfo pi);
static Leaf	read_num(ParseInfo pi);
static Leaf	read_true(ParseInfo pi);
static Leaf	read_false(ParseInfo pi);
static Leaf	read_nil(ParseInfo pi);
static void	next_non_white(ParseInfo pi);
static char*	read_quoted_value(ParseInfo pi);
static void	skip_comment(ParseInfo pi);

static VALUE	protect_open_proc(VALUE x);
static VALUE	parse_json(VALUE clas, char *json, int given, int allocated);
static void	each_leaf(Doc doc, VALUE self);
static int	move_step(Doc doc, const char *path, int loc);
static Leaf	get_doc_leaf(Doc doc, const char *path);
static Leaf	get_leaf(Leaf *stack, Leaf *lp, const char *path);
static void	each_value(Doc doc, Leaf leaf);

static void	doc_init(Doc doc);
static void	doc_free(Doc doc);
static VALUE	doc_open(VALUE clas, VALUE str);
static VALUE	doc_open_file(VALUE clas, VALUE filename);
static VALUE	doc_where(VALUE self);
static VALUE	doc_local_key(VALUE self);
static VALUE	doc_home(VALUE self);
static VALUE	doc_type(int argc, VALUE *argv, VALUE self);
static VALUE	doc_fetch(int argc, VALUE *argv, VALUE self);
static VALUE	doc_each_leaf(int argc, VALUE *argv, VALUE self);
static VALUE	doc_move(VALUE self, VALUE str);
static VALUE	doc_each_child(int argc, VALUE *argv, VALUE self);
static VALUE	doc_each_value(int argc, VALUE *argv, VALUE self);
static VALUE	doc_dump(int argc, VALUE *argv, VALUE self);
static VALUE	doc_size(VALUE self);

VALUE	oj_doc_class = 0;

inline static void
next_non_white(ParseInfo pi) {
    for (; 1; pi->s++) {
	switch(*pi->s) {
	case ' ':
	case '\t':
	case '\f':
	case '\n':
	case '\r':
	    break;
	case '/':
	    skip_comment(pi);
	    break;
	default:
	    return;
	}
    }
}

inline static void
next_white(ParseInfo pi) {
    for (; 1; pi->s++) {
	switch(*pi->s) {
	case ' ':
	case '\t':
	case '\f':
	case '\n':
	case '\r':
	case '\0':
	    return;
	default:
	    break;
	}
    }
}

inline static char*
ulong_fill(char *s, size_t num) {
    char	buf[32];
    char	*b = buf + sizeof(buf) - 1;

    *b-- = '\0';
    for (; 0 < num; num /= 10, b--) {
	*b = (num % 10) + '0';
    }
    b++;
    if ('\0' == *b) {
	b--;
	*b = '0';
    }
    for (; '\0' != *b; b++, s++) {
	*s = *b;
    }
    return s;
}

inline static void
leaf_init(Leaf leaf, int type) {
    leaf->next = 0;
    leaf->type = type;
    leaf->parent_type = T_NONE;
    switch (type) {
    case T_ARRAY:
    case T_HASH:
	leaf->elements = 0;
	leaf->value_type = COL_VAL;
	break;
    case T_NIL:
	leaf->value = Qnil;
	leaf->value_type = RUBY_VAL;
	break;
    case T_TRUE:
	leaf->value = Qtrue;
	leaf->value_type = RUBY_VAL;
	break;
    case T_FALSE:
	leaf->value = Qfalse;
	leaf->value_type = RUBY_VAL;
	break;
    case T_FIXNUM:
    case T_FLOAT:
    case T_STRING:
    default:
	leaf->value_type = STR_VAL;
	break;
    }
}

inline static Leaf
leaf_new(Doc doc, int type) {
    Leaf	leaf;

    if (0 == doc->batches || BATCH_SIZE == doc->batches->next_avail) {
	Batch	b = ALLOC(struct _Batch);

	b->next = doc->batches;
	doc->batches = b;
	b->next_avail = 0;
    }
    leaf = &doc->batches->leaves[doc->batches->next_avail];
    doc->batches->next_avail++;
    leaf_init(leaf, type);

    return leaf;
}

inline static void
leaf_append_element(Leaf parent, Leaf element) {
    if (0 == parent->elements) {
	parent->elements = element;
	element->next = element;
    } else {
	element->next = parent->elements->next;
	parent->elements->next = element;
	parent->elements = element;
    }
}

static VALUE
leaf_value(Doc doc, Leaf leaf) {
    if (RUBY_VAL != leaf->value_type) {
	switch (leaf->type) {
	case T_NIL:
	    leaf->value = Qnil;
	    break;
	case T_TRUE:
	    leaf->value = Qtrue;
	    break;
	case T_FALSE:
	    leaf->value = Qfalse;
	    break;
	case T_FIXNUM:
	    leaf_fixnum_value(leaf);
	    break;
	case T_FLOAT:
	    leaf_float_value(leaf);
	    break;
	case T_STRING:
	    leaf->value = rb_str_new2(leaf->str);
#if HAS_ENCODING_SUPPORT
	    rb_enc_associate(leaf->value, oj_utf8_encoding);
#endif
	    leaf->value_type = RUBY_VAL;
	    break;
	case T_ARRAY:
	    return leaf_array_value(doc, leaf);
	    break;
	case T_HASH:
	    return leaf_hash_value(doc, leaf);
	    break;
	default:
	    rb_raise(rb_eTypeError, "Unexpected type %02x.", leaf->type);
	    break;
	}
    }
    return leaf->value;
}

inline static Doc
self_doc(VALUE self) {
    Doc	doc = DATA_PTR(self);

    if (0 == doc) {
	rb_raise(rb_eIOError, "Document already closed or not open.");
    }
    return doc;
}

static void
skip_comment(ParseInfo pi) {
    pi->s++; // skip first /
    if ('*' == *pi->s) {
	pi->s++;
	for (; '\0' != *pi->s; pi->s++) {
	    if ('*' == *pi->s && '/' == *(pi->s + 1)) {
		pi->s++;
		return;
	    } else if ('\0' == *pi->s) {
		raise_error("comment not terminated", pi->str, pi->s);
	    }
	}
    } else if ('/' == *pi->s) {
	for (; 1; pi->s++) {
	    switch (*pi->s) {
	    case '\n':
	    case '\r':
	    case '\f':
	    case '\0':
		return;
	    default:
		break;
	    }
	}
    } else {
	raise_error("invalid comment", pi->str, pi->s);
    }
}

#ifdef RUBINIUS_RUBY
#define NUM_MAX 0x07FFFFFF
#else
#define NUM_MAX (FIXNUM_MAX >> 8)
#endif


static void
leaf_fixnum_value(Leaf leaf) {
    char	*s = leaf->str;
    int64_t	n = 0;
    int		neg = 0;
    int		big = 0;
    
    if ('-' == *s) {
	s++;
	neg = 1;
    } else if ('+' == *s) {
	s++;
    }
    for (; '0' <= *s && *s <= '9'; s++) {
	n = n * 10 + (*s - '0');
	if (NUM_MAX <= n) {
	    big = 1;
	}
    }
    if (big) {
	char	c = *s;
	
	*s = '\0';
	leaf->value = rb_cstr_to_inum(leaf->str, 10, 0);
	*s = c;
    } else {
	if (neg) {
	    n = -n;
	}
	leaf->value = LONG2NUM(n);
    }
    leaf->value_type = RUBY_VAL;
}

#ifdef JRUBY_RUBY
static void
leaf_float_value(Leaf leaf) {
    char	*s = leaf->str;
    int64_t	n = 0;
    long	a = 0;
    long	div = 1;
    long	e = 0;
    int		neg = 0;
    int		eneg = 0;
    int		big = 0;

    if ('-' == *s) {
	s++;
	neg = 1;
    } else if ('+' == *s) {
	s++;
    }
    for (; '0' <= *s && *s <= '9'; s++) {
	n = n * 10 + (*s - '0');
	if (NUM_MAX <= n) {
	    big = 1;
	}
    }
    if (big) {
	char	c = *s;
	
	*s = '\0';
	leaf->value = rb_cstr_to_inum(leaf->str, 10, 0);
	*s = c;
    } else {
	double	d;

	if ('.' == *s) {
	    s++;
	    for (; '0' <= *s && *s <= '9'; s++) {
		a = a * 10 + (*s - '0');
		div *= 10;
	    }
	}
	if ('e' == *s || 'E' == *s) {
	    s++;
	    if ('-' == *s) {
		s++;
		eneg = 1;
	    } else if ('+' == *s) {
		s++;
	    }
	    for (; '0' <= *s && *s <= '9'; s++) {
		e = e * 10 + (*s - '0');
	    }
	}
	d = (double)n + (double)a / (double)div;
	if (neg) {
	    d = -d;
	}
	if (0 != e) {
	    if (eneg) {
		e = -e;
	    }
	    d *= pow(10.0, e);
	}
	leaf->value = rb_float_new(d);
    }
    leaf->value_type = RUBY_VAL;
}
#else
static void
leaf_float_value(Leaf leaf) {
    leaf->value = rb_float_new(rb_cstr_to_dbl(leaf->str, 1));
    leaf->value_type = RUBY_VAL;
}
#endif

static VALUE
leaf_array_value(Doc doc, Leaf leaf) {
    VALUE	a = rb_ary_new();

    if (0 != leaf->elements) {
	Leaf	first = leaf->elements->next;
	Leaf	e = first;

	do {
	    rb_ary_push(a, leaf_value(doc, e));
	    e = e->next;
	} while (e != first);
    }
    return a;
}

static VALUE
leaf_hash_value(Doc doc, Leaf leaf) {
    VALUE	h = rb_hash_new();

    if (0 != leaf->elements) {
	Leaf	first = leaf->elements->next;
	Leaf	e = first;
	VALUE	key;

	do {
	    key = rb_str_new2(e->key);
#if HAS_ENCODING_SUPPORT
	    rb_enc_associate(key, oj_utf8_encoding);
#endif
	    rb_hash_aset(h, key, leaf_value(doc, e));
	    e = e->next;
	} while (e != first);
    }
    return h;
}

static Leaf
read_next(ParseInfo pi) {
    Leaf	leaf = 0;

    next_non_white(pi);	// skip white space
    switch (*pi->s) {
    case '{':
	leaf = read_obj(pi);
	break;
    case '[':
	leaf = read_array(pi);
	break;
    case '"':
	leaf = read_str(pi);
	break;
    case '+':
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
	leaf = read_num(pi);
	break;
    case 't':
	leaf = read_true(pi);
	break;
    case 'f':
	leaf = read_false(pi);
	break;
    case 'n':
	leaf = read_nil(pi);
	break;
    case '\0':
    default:
	break; // returns 0
    }
    pi->doc->size++;

    return leaf;
}

static Leaf
read_obj(ParseInfo pi) {
    Leaf	h = leaf_new(pi->doc, T_HASH);
    char	*end;
    const char	*key = 0;
    Leaf	val = 0;

    pi->s++;
    next_non_white(pi);
    if ('}' == *pi->s) {
	pi->s++;
	return h;
    }
    while (1) {
	next_non_white(pi);
	key = 0;
	val = 0;
	if ('"' != *pi->s || 0 == (key = read_quoted_value(pi))) {
	    raise_error("unexpected character", pi->str, pi->s);
	}
	next_non_white(pi);
	if (':' == *pi->s) {
	    pi->s++;
	} else {
	    raise_error("invalid format, expected :", pi->str, pi->s);
	}
	if (0 == (val = read_next(pi))) {
	    //printf("*** '%s'\n", pi->s);
	    raise_error("unexpected character", pi->str, pi->s);
	}
	end = pi->s;
	val->key = key;
	val->parent_type = T_HASH;
	leaf_append_element(h, val);
	next_non_white(pi);
	if ('}' == *pi->s) {
	    pi->s++;
	    *end = '\0';
	    break;
	} else if (',' == *pi->s) {
	    pi->s++;
	} else {
	    printf("*** '%s'\n", pi->s);
	    raise_error("invalid format, expected , or } while in an object", pi->str, pi->s);
	}
	*end = '\0';
    }
    return h;
}

static Leaf
read_array(ParseInfo pi) {
    Leaf	a = leaf_new(pi->doc, T_ARRAY);
    Leaf	e;
    char	*end;
    int		cnt = 0;

    pi->s++;
    next_non_white(pi);
    if (']' == *pi->s) {
	pi->s++;
	return a;
    }
    while (1) {
	next_non_white(pi);
	if (0 == (e = read_next(pi))) {
	    raise_error("unexpected character", pi->str, pi->s);
	}
	cnt++;
	e->index = cnt;
	e->parent_type = T_ARRAY;
	leaf_append_element(a, e);
	end = pi->s;
	next_non_white(pi);
	if (',' == *pi->s) {
	    pi->s++;
	} else if (']' == *pi->s) {
	    pi->s++;
	    *end = '\0';
	    break;
	} else {
	    raise_error("invalid format, expected , or ] while in an array", pi->str, pi->s);
	}
	*end = '\0';
    }
    return a;
}

static Leaf
read_str(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, T_STRING);

    leaf->str = read_quoted_value(pi);

    return leaf;
}

static Leaf
read_num(ParseInfo pi) {
    char	*start = pi->s;
    int		type = T_FIXNUM;
    Leaf	leaf;

    if ('-' == *pi->s) {
	pi->s++;
    }
    // digits
    for (; '0' <= *pi->s && *pi->s <= '9'; pi->s++) {
    }
    if ('.' == *pi->s) {
	type = T_FLOAT;
	pi->s++;
	for (; '0' <= *pi->s && *pi->s <= '9'; pi->s++) {
	}
    }
    if ('e' == *pi->s || 'E' == *pi->s) {
	pi->s++;
	if ('-' == *pi->s || '+' == *pi->s) {
	    pi->s++;
	}
	for (; '0' <= *pi->s && *pi->s <= '9'; pi->s++) {
	}
    }
    leaf = leaf_new(pi->doc, type);
    leaf->str = start;

    return leaf;
}

static Leaf
read_true(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, T_TRUE);

    pi->s++;
    if ('r' != *pi->s || 'u' != *(pi->s + 1) || 'e' != *(pi->s + 2)) {
	raise_error("invalid format, expected 'true'", pi->str, pi->s);
    }
    pi->s += 3;

    return leaf;
}

static Leaf
read_false(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, T_FALSE);

    pi->s++;
    if ('a' != *pi->s || 'l' != *(pi->s + 1) || 's' != *(pi->s + 2) || 'e' != *(pi->s + 3)) {
	raise_error("invalid format, expected 'false'", pi->str, pi->s);
    }
    pi->s += 4;

    return leaf;
}

static Leaf
read_nil(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, T_NIL);

    pi->s++;
    if ('u' != *pi->s || 'l' != *(pi->s + 1) || 'l' != *(pi->s + 2)) {
	raise_error("invalid format, expected 'nil'", pi->str, pi->s);
    }
    pi->s += 3;

    return leaf;
}

static char
read_hex(ParseInfo pi, char *h) {
    uint8_t	b = 0;

    if ('0' <= *h && *h <= '9') {
	b = *h - '0';
    } else if ('A' <= *h && *h <= 'F') {
	b = *h - 'A' + 10;
    } else if ('a' <= *h && *h <= 'f') {
	b = *h - 'a' + 10;
    } else {
	pi->s = h;
	raise_error("invalid hex character", pi->str, pi->s);
    }
    h++;
    b = b << 4;
    if ('0' <= *h && *h <= '9') {
	b += *h - '0';
    } else if ('A' <= *h && *h <= 'F') {
	b += *h - 'A' + 10;
    } else if ('a' <= *h && *h <= 'f') {
	b += *h - 'a' + 10;
    } else {
	pi->s = h;
	raise_error("invalid hex character", pi->str, pi->s);
    }
    return (char)b;
}

/* Assume the value starts immediately and goes until the quote character is
 * reached again. Do not read the character after the terminating quote.
 */
static char*
read_quoted_value(ParseInfo pi) {
    char	*value = 0;
    char	*h = pi->s; // head
    char	*t = h;	    // tail
    
    h++;	// skip quote character
    t++;
    value = h;
    for (; '"' != *h; h++, t++) {
	if ('\0' == *h) {
	    pi->s = h;
	    raise_error("quoted string not terminated", pi->str, pi->s);
	} else if ('\\' == *h) {
	    h++;
	    switch (*h) {
	    case 'n':	*t = '\n';	break;
	    case 'r':	*t = '\r';	break;
	    case 't':	*t = '\t';	break;
	    case 'f':	*t = '\f';	break;
	    case 'b':	*t = '\b';	break;
	    case '"':	*t = '"';	break;
	    case '/':	*t = '/';	break;
	    case '\\':	*t = '\\';	break;
	    case 'u':
		h++;
		*t = read_hex(pi, h);
		h += 2;
		if ('\0' != *t) {
		    t++;
		}
		*t = read_hex(pi, h);
		h++;
		break;
	    default:
		pi->s = h;
		raise_error("invalid escaped character", pi->str, pi->s);
		break;
	    }
	} else if (t != h) {
	    *t = *h;
	}
    }
    *t = '\0'; // terminate value
    pi->s = h + 1;

    return value;
}

// doc support functions
inline static void
doc_init(Doc doc) {
    doc->where = doc->where_path;
    *doc->where = 0;
    doc->data = 0;
    doc->self = Qundef;
    doc->size = 0;
    doc->json = 0;
    doc->batches = &doc->batch0;
    doc->batch0.next = 0;
    doc->batch0.next_avail = 0;
}

static void
doc_free(Doc doc) {
    if (0 != doc) {
	Batch	b;

	while (0 != (b = doc->batches)) {
	    doc->batches = doc->batches->next;
	    if (&doc->batch0 != b) {
		xfree(b);
	    }
	}
	//xfree(f);
    }
}

static VALUE
protect_open_proc(VALUE x) {
    ParseInfo	pi = (ParseInfo)x;

    pi->doc->data = read_next(pi); // parse
    *pi->doc->where = pi->doc->data;
    pi->doc->where = pi->doc->where_path;
    if (rb_block_given_p()) {
	return rb_yield(pi->doc->self); // caller processing
    }
    return Qnil;
}

static void
free_doc_cb(void *x) {
    Doc	doc = (Doc)x;

    if (0 != doc) {
	xfree(doc->json);
	doc_free(doc);
    }
}

static VALUE
parse_json(VALUE clas, char *json, int given, int allocated) {
    struct _ParseInfo	pi;
    VALUE		result = Qnil;
    Doc			doc;
    int			ex = 0;

    if (given) {
	doc = ALLOCA_N(struct _Doc, 1);
    } else {
	doc = ALLOC_N(struct _Doc, 1);
    }
    pi.str = json;
    pi.s = pi.str;
    doc_init(doc);
    pi.doc = doc;
    // last arg is free func void* func(void*)
    doc->self = rb_data_object_alloc(clas, doc, 0, free_doc_cb);
    doc->json = json;
    DATA_PTR(doc->self) = doc;
    result = rb_protect(protect_open_proc, (VALUE)&pi, &ex);
    if (given || 0 != ex) {
	DATA_PTR(doc->self) = 0;
	doc_free(pi.doc);
	if (allocated) {
	    xfree(json);
	}
    } else {
	result = doc->self;
    }
    if (0 != ex) {
	rb_jump_tag(ex);
    }
    return result;
}

static Leaf
get_doc_leaf(Doc doc, const char *path) {
    Leaf	leaf = *doc->where;

    if (0 != doc->data && 0 != path) {
	Leaf	stack[MAX_STACK];
	Leaf	*lp;

	if ('/' == *path) {
	    path++;
	    *stack = doc->data;
	    lp = stack;
	} else if (doc->where == doc->where_path) {
	    *stack = doc->data;
	    lp = stack;
	} else {
	    size_t	cnt = doc->where - doc->where_path;

	    memcpy(stack, doc->where_path, sizeof(Leaf) * cnt);
	    lp = stack + cnt;
	}
	return get_leaf(stack, lp, path);
    }
    return leaf;
}

static Leaf
get_leaf(Leaf *stack, Leaf *lp, const char *path) {
    Leaf	leaf = *lp;

    if (MAX_STACK <= lp - stack) {
	rb_raise(rb_eIOError, "Path too deep. limit is %d levels.\n", MAX_STACK);
    }
    if ('\0' != *path) {
	if ('.' == *path && '.' == *(path + 1)) {
	    path += 2;
	    if ('/' == *path) {
		path++;
	    }
	    if (stack < lp) {
		leaf = get_leaf(stack, lp - 1, path);
	    } else {
		return 0;
	    }
	} else if (COL_VAL == leaf->value_type && 0 != leaf->elements) {
	    Leaf	first = leaf->elements->next;
	    Leaf	e = first;
	    int		type = leaf->type;

	    leaf = 0;
	    if (T_ARRAY == type) {
		int	cnt = 0;

		for (; '0' <= *path && *path <= '9'; path++) {
		    cnt = cnt * 10 + (*path - '0');
		}
		if ('/' == *path) {
		    path++;
		}
		do {
		    if (1 >= cnt) {
			lp++;
			*lp = e;
			leaf = get_leaf(stack, lp, path);
			break;
		    }
		    cnt--;
		    e = e->next;
		} while (e != first);
	    } else if (T_HASH == type) {
		const char	*key = path;
		const char	*slash = strchr(path, '/');
		int		klen;

		if (0 == slash) {
		    klen = (int)strlen(key);
		    path += klen;
		} else {
		    klen = (int)(slash - key);
		    path += klen + 1;
		}
		do {
		    if (0 == strncmp(key, e->key, klen) && '\0' == e->key[klen]) {
			lp++;
			*lp = e;
			leaf = get_leaf(stack, lp, path);
			break;
		    }
		    e = e->next;
		} while (e != first);
	    }
	}
    }
    return leaf;
}

static void
each_leaf(Doc doc, VALUE self) {
    if (COL_VAL == (*doc->where)->value_type) {
	if (0 != (*doc->where)->elements) {
	    Leaf	first = (*doc->where)->elements->next;
	    Leaf	e = first;

	    doc->where++;
	    do {
		*doc->where = e;
		each_leaf(doc, self);
		e = e->next;
	    } while (e != first);
	}
    } else {
	rb_yield(self);
    }
}

static int
move_step(Doc doc, const char *path, int loc) {
    if (MAX_STACK <= doc->where - doc->where_path) {
	rb_raise(rb_eIOError, "Path too deep. limit is %d levels.\n", MAX_STACK);
    }
    if ('\0' == *path) {
	loc = 0;
    } else {
	Leaf	leaf;

	if (0 == doc->where || 0 == (leaf = *doc->where)) {
	    printf("*** Internal error at %s\n", path);
	    return loc;
	}
	if ('.' == *path && '.' == *(path + 1)) {
	    Leaf	init = *doc->where;

	    path += 2;
	    if (doc->where == doc->where_path) {
		return loc;
	    }
	    if ('/' == *path) {
		path++;
	    }
	    *doc->where = 0;
	    doc->where--;
	    loc = move_step(doc, path, loc + 1);
	    if (0 != loc) {
		*doc->where = init;
		doc->where++;
	    }
	} else if (COL_VAL == leaf->value_type && 0 != leaf->elements) {
	    Leaf	first = leaf->elements->next;
	    Leaf	e = first;

	    if (T_ARRAY == leaf->type) {
		int	cnt = 0;

		for (; '0' <= *path && *path <= '9'; path++) {
		    cnt = cnt * 10 + (*path - '0');
		}
		if ('/' == *path) {
		    path++;
		} else if ('\0' != *path) {
		    return loc;
		}
		do {
		    if (1 >= cnt) {
			doc->where++;
			*doc->where = e;
			loc = move_step(doc, path, loc + 1);
			if (0 != loc) {
			    *doc->where = 0;
			    doc->where--;
			}
			break;
		    }
		    cnt--;
		    e = e->next;
		} while (e != first);
	    } else if (T_HASH == leaf->type) {
		const char	*key = path;
		const char	*slash = strchr(path, '/');
		int		klen;

		if (0 == slash) {
		    klen = (int)strlen(key);
		    path += klen;
		} else {
		    klen = (int)(slash - key);
		    path += klen + 1;
		}
		do {
		    if (0 == strncmp(key, e->key, klen) && '\0' == e->key[klen]) {
			doc->where++;
			*doc->where = e;
			loc = move_step(doc, path, loc + 1);
			if (0 != loc) {
			    *doc->where = 0;
			    doc->where--;
			}
			break;
		    }
		    e = e->next;
		} while (e != first);
	    }
	}
    }
    return loc;
}

static void
each_value(Doc doc, Leaf leaf) {
    if (COL_VAL == leaf->value_type) {
	if (0 != leaf->elements) {
	    Leaf	first = leaf->elements->next;
	    Leaf	e = first;

	    do {
		each_value(doc, e);
		e = e->next;
	    } while (e != first);
	}
    } else {
	rb_yield(leaf_value(doc, leaf));
    }
}

// doc functions

/* call-seq: open(json) { |doc| ... } => Object
 *
 * Parses a JSON document String and then yields to the provided block if one
 * is given with an instance of the Oj::Doc as the single yield parameter. If
 * a block is not given then an Oj::Doc instance is returned and must be
 * closed with a call to the #close() method when no longer needed.
 *
 * @param [String] json JSON document string
 * @yieldparam [Oj::Doc] doc parsed JSON document
 * @yieldreturn [Object] returns the result of the yield as the result of the method call
 * @example
 *   Oj::Doc.open('[1,2,3]') { |doc| doc.size() }  #=> 4
 *   # or as an alternative
 *   doc = Oj::Doc.open('[1,2,3]')
 *   doc.size()  #=> 4
 *   doc.close()
 */
static VALUE
doc_open(VALUE clas, VALUE str) {
    char	*json;
    size_t	len;
    VALUE	obj;
    int		given = rb_block_given_p();
    int		allocate;

    Check_Type(str, T_STRING);
    len = RSTRING_LEN(str) + 1;
    allocate = (SMALL_XML < len || !given);
    if (allocate) {
	json = ALLOC_N(char, len);
    } else {
	json = ALLOCA_N(char, len);
    }
    memcpy(json, StringValuePtr(str), len);
    obj = parse_json(clas, json, given, allocate);
    if (given && allocate) {
	xfree(json);
    }
    return obj;
}

/* call-seq: open_file(filename) { |doc| ... } => Object
 *
 * Parses a JSON document from a file and then yields to the provided block if
 * one is given with an instance of the Oj::Doc as the single yield
 * parameter. If a block is not given then an Oj::Doc instance is returned and
 * must be closed with a call to the #close() method when no longer needed.
 *
 * @param [String] filename name of file that contains a JSON document
 * @yieldparam [Oj::Doc] doc parsed JSON document
 * @yieldreturn [Object] returns the result of the yield as the result of the method call
 * @example
 *   File.open('array.json', 'w') { |f| f.write('[1,2,3]') }
 *   Oj::Doc.open_file(filename) { |doc| doc.size() }  #=> 4
 *   # or as an alternative
 *   doc = Oj::Doc.open_file(filename)
 *   doc.size()  #=> 4
 *   doc.close()
 */
static VALUE
doc_open_file(VALUE clas, VALUE filename) {
    char	*path;
    char	*json;
    FILE	*f;
    size_t	len;
    VALUE	obj;
    int		given = rb_block_given_p();
    int		allocate;

    Check_Type(filename, T_STRING);
    path = StringValuePtr(filename);
    if (0 == (f = fopen(path, "r"))) {
	rb_raise(rb_eIOError, "%s\n", strerror(errno));
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    allocate = (SMALL_XML < len || !given);
    if (allocate) {
	json = ALLOC_N(char, len + 1);
    } else {
	json = ALLOCA_N(char, len + 1);
    }
    fseek(f, 0, SEEK_SET);
    if (len != fread(json, 1, len, f)) {
	fclose(f);
	rb_raise(rb_eLoadError, "Failed to read %lu bytes from %s.\n", (unsigned long)len, path);
    }
    fclose(f);
    json[len] = '\0';
    obj = parse_json(clas, json, given, allocate);
    if (given && allocate) {
	xfree(json);
    }
    return obj;
}

/* Document-method: parse
 * @see Oj::Doc.open
 */

/* call-seq: where?() => String
 *
 * Returns a String that describes the absolute path to the current location
 * in the JSON document.
 */
static VALUE
doc_where(VALUE self) {
    Doc	doc = self_doc(self);

    if (0 == *doc->where_path || doc->where == doc->where_path) {
	return oj_slash_string;
    } else {
	Leaf	*lp;
	Leaf	leaf;
	size_t	size = 3; // leading / and terminating \0
	char	*path;
	char	*p;

	for (lp = doc->where_path; lp <= doc->where; lp++) {
	    leaf = *lp;
	    if (T_HASH == leaf->parent_type) {
		size += strlen((*lp)->key) + 1;
	    } else if (T_ARRAY == leaf->parent_type) {
		size += ((*lp)->index < 100) ? 3 : 11;
	    }
	}
	path = ALLOCA_N(char, size);
	p = path;
	for (lp = doc->where_path; lp <= doc->where; lp++) {
	    leaf = *lp;
	    if (T_HASH == leaf->parent_type) {
		p = stpcpy(p, (*lp)->key);
	    } else if (T_ARRAY == leaf->parent_type) {
		p = ulong_fill(p, (*lp)->index);
	    }
	    *p++ = '/';
	}
	*--p = '\0';
	return rb_str_new(path, p - path);
    }
}

/* call-seq: local_key() => String, Fixnum, nil
 *
 * Returns the final key to the current location.
 * @example
 *   Oj::Doc.open('[1,2,3]') { |doc| doc.move('/2'); doc.local_key() }	    #=> 2
 *   Oj::Doc.open('{"one":3}') { |doc| doc.move('/one'); doc.local_key() }  #=> "one"
 *   Oj::Doc.open('[1,2,3]') { |doc| doc.local_key() }			    #=> nil
 */
static VALUE
doc_local_key(VALUE self) {
    Doc		doc = self_doc(self);
    Leaf	leaf = *doc->where;
    VALUE	key = Qnil;

    if (T_HASH == leaf->parent_type) {
	key = rb_str_new2(leaf->key);
#if HAS_ENCODING_SUPPORT
	rb_enc_associate(key, oj_utf8_encoding);
#endif
    } else if (T_ARRAY == leaf->parent_type) {
	key = LONG2NUM(leaf->index);
    }
    return key;
}

/* call-seq: home() => nil
 *
 * Moves the document marker or location to the hoot or home position. The
 * same operation can be performed with a Oj::Doc.move('/').
 * @example
 *   Oj::Doc.open('[1,2,3]') { |doc| doc.move('/2'); doc.home(); doc.where? }  #=> '/'
 */
static VALUE
doc_home(VALUE self) {
    Doc	doc = self_doc(self);

    *doc->where_path = doc->data;
    doc->where = doc->where_path;

    return oj_slash_string;
}

/* call-seq: type(path=nil) => Class
 *
 * Returns the Class of the data value at the location identified by the path
 * or the current location if the path is nil or not provided. This method
 * does not create the Ruby Object at the location specified so the overhead
 * is low.
 * @param [String] path path to the location to get the type of if provided
 * @example
 *   Oj::Doc.open('[1,2]') { |doc| doc.type() }	     #=> Array
 *   Oj::Doc.open('[1,2]') { |doc| doc.type('/1') }  #=> Fixnum
 */
static VALUE
doc_type(int argc, VALUE *argv, VALUE self) {
    Doc		doc = self_doc(self);
    Leaf	leaf;
    const char	*path = 0;
    VALUE	type = Qnil;

    if (1 <= argc) {
	Check_Type(*argv, T_STRING);
	path = StringValuePtr(*argv);
    }
    if (0 != (leaf = get_doc_leaf(doc, path))) {
	switch (leaf->type) {
	case T_NIL:	type = rb_cNilClass;	break;
	case T_TRUE:	type = rb_cTrueClass;	break;
	case T_FALSE:	type = rb_cFalseClass;	break;
	case T_STRING:	type = rb_cString;	break;
	case T_FIXNUM:	type = rb_cFixnum;	break;
	case T_FLOAT:	type = rb_cFloat;	break;
	case T_ARRAY:	type = rb_cArray;	break;
	case T_HASH:	type = rb_cHash;	break;
	default:				break;
	}
    }
    return type;
}

/* call-seq: fetch(path=nil) => nil, true, false, Fixnum, Float, String, Array, Hash
 *
 * Returns the value at the location identified by the path or the current
 * location if the path is nil or not provided. This method will create and
 * return an Array or Hash if that is the type of Object at the location
 * specified. This is more expensive than navigating to the leaves of the JSON
 * document.
 * @param [String] path path to the location to get the type of if provided
 * @example
 *   Oj::Doc.open('[1,2]') { |doc| doc.fetch() }      #=> [1, 2]
 *   Oj::Doc.open('[1,2]') { |doc| doc.fetch('/1') }  #=> 1
 */
static VALUE
doc_fetch(int argc, VALUE *argv, VALUE self) {
    Doc		doc = self_doc(self);
    Leaf	leaf;
    VALUE	val = Qnil;
    const char	*path = 0;

    if (1 <= argc) {
	Check_Type(*argv, T_STRING);
	path = StringValuePtr(*argv);
	if (2 == argc) {
	    val = argv[1];
	}
    }
    if (0 != (leaf = get_doc_leaf(doc, path))) {
	val = leaf_value(doc, leaf);
    }
    return val;
}

/* call-seq: each_leaf(path=nil) => nil
 *
 * Yields to the provided block for each leaf node with the identified
 * location of the JSON document as the root. The parameter passed to the
 * block on yield is the Doc instance after moving to the child location.
 * @param [String] path if provided it identified the top of the branch to process the leaves of
 * @yieldparam [Doc] Doc at the child location
 * @example
 *   Oj::Doc.open('[3,[2,1]]') { |doc|
 *       result = {}
 *       doc.each_leaf() { |d| result[d.where?] = d.fetch() }
 *       result
 *   }
 *   #=> ["/1" => 3, "/2/1" => 2, "/2/2" => 1]
 */
static VALUE
doc_each_leaf(int argc, VALUE *argv, VALUE self) {
    if (rb_block_given_p()) {
	Leaf		save_path[MAX_STACK];
	Doc		doc = self_doc(self);
	const char	*path = 0;
	size_t		wlen;

	wlen = doc->where - doc->where_path;
	if (0 < wlen) {
	    memcpy(save_path, doc->where_path, sizeof(Leaf) * wlen);
	}
	if (1 <= argc) {
	    Check_Type(*argv, T_STRING);
	    path = StringValuePtr(*argv);
	    if ('/' == *path) {
		doc->where = doc->where_path;
		path++;
	    }
	    if (0 != move_step(doc, path, 1)) {
		if (0 < wlen) {
		    memcpy(doc->where_path, save_path, sizeof(Leaf) * wlen);
		}
		return Qnil;
	    }
	}
	each_leaf(doc, self);
	if (0 < wlen) {
	    memcpy(doc->where_path, save_path, sizeof(Leaf) * wlen);
	}
    }
    return Qnil;
}

/* call-seq: move(path) => nil
 *
 * Moves the document marker to the path specified. The path can an absolute
 * path or a relative path.
 * @param [String] path path to the location to move to
 * @example
 *   Oj::Doc.open('{"one":[1,2]') { |doc| doc.move('/one/2'); doc.where? }  #=> "/one/2"
 */
static VALUE
doc_move(VALUE self, VALUE str) {
    Doc		doc = self_doc(self);
    const char	*path;
    int		loc;

    Check_Type(str, T_STRING);
    path = StringValuePtr(str);
    if ('/' == *path) {
	doc->where = doc->where_path;
	path++;
    }
    if (0 != (loc = move_step(doc, path, 1))) {
	rb_raise(rb_eArgError, "Failed to locate element %d of the path %s.", loc, path);
    }
    return Qnil;
}

/* call-seq: each_child(path=nil) { |doc| ... } => nil
 *
 * Yields to the provided block for each immediate child node with the
 * identified location of the JSON document as the root. The parameter passed
 * to the block on yield is the Doc instance after moving to the child
 * location.
 * @param [String] path if provided it identified the top of the branch to process the chilren of
 * @yieldparam [Doc] Doc at the child location
 * @example
 *   Oj::Doc.open('[3,[2,1]]') { |doc|
 *       result = []
 *       doc.each_value('/2') { |doc| result << doc.where? }
 *       result
 *   }
 *   #=> ["/2/1", "/2/2"]
 */
static VALUE
doc_each_child(int argc, VALUE *argv, VALUE self) {
    if (rb_block_given_p()) {
	Leaf		save_path[MAX_STACK];
	Doc		doc = self_doc(self);
	const char	*path = 0;
	size_t		wlen;

	wlen = doc->where - doc->where_path;
	if (0 < wlen) {
	    memcpy(save_path, doc->where_path, sizeof(Leaf) * wlen);
	}
	if (1 <= argc) {
	    Check_Type(*argv, T_STRING);
	    path = StringValuePtr(*argv);
	    if ('/' == *path) {
		doc->where = doc->where_path;
		path++;
	    }
	    if (0 != move_step(doc, path, 1)) {
		if (0 < wlen) {
		    memcpy(doc->where_path, save_path, sizeof(Leaf) * wlen);
		}
		return Qnil;
	    }
	}
	if (COL_VAL == (*doc->where)->value_type && 0 != (*doc->where)->elements) {
	    Leaf	first = (*doc->where)->elements->next;
	    Leaf	e = first;

	    doc->where++;
	    do {
		*doc->where = e;
		rb_yield(self);
		e = e->next;
	    } while (e != first);
	}
	if (0 < wlen) {
	    memcpy(doc->where_path, save_path, sizeof(Leaf) * wlen);
	}
    }
    return Qnil;
}

/* call-seq: each_value(path=nil) { |val| ... } => nil
 *
 * Yields to the provided block for each leaf value in the identified location
 * of the JSON document. The parameter passed to the block on yield is the
 * value of the leaf. Only those leaves below the element specified by the
 * path parameter are processed.
 * @param [String] path if provided it identified the top of the branch to process the leaf values of
 * @yieldparam [Object] val each leaf value
 * @example
 *   Oj::Doc.open('[3,[2,1]]') { |doc|
 *       result = []
 *       doc.each_value() { |v| result << v }
 *       result
 *   }
 *   #=> [3, 2, 1]
 *   
 *   Oj::Doc.open('[3,[2,1]]') { |doc|
 *       result = []
 *       doc.each_value('/2') { |v| result << v }
 *       result
 *   }
 *   #=> [2, 1]
 */
static VALUE
doc_each_value(int argc, VALUE *argv, VALUE self) {
    if (rb_block_given_p()) {
	Doc		doc = self_doc(self);
	const char	*path = 0;
	Leaf		leaf;

	if (1 <= argc) {
	    Check_Type(*argv, T_STRING);
	    path = StringValuePtr(*argv);
	}
	if (0 != (leaf = get_doc_leaf(doc, path))) {
	    each_value(doc, leaf);
	}
    }
    return Qnil;
}

/* call-seq: dump(path=nil) => String
 *
 * Dumps the document or nodes to a new JSON document. It uses the default
 * options for generating the JSON.
 * @param [String] path if provided it identified the top of the branch to dump to JSON
 * @param [String] filename if provided it is the filename to write the output to
 * @example
 *   Oj::Doc.open('[3,[2,1]]') { |doc|
 *       doc.dump('/2')
 *   }
 *   #=> "[2,1]"
 */
static VALUE
doc_dump(int argc, VALUE *argv, VALUE self) {
    Doc		doc = self_doc(self);
    Leaf	leaf;
    const char	*path = 0;
    const char	*filename = 0;

    if (1 <= argc) {
	if (Qnil != *argv) {
	    Check_Type(*argv, T_STRING);
	    path = StringValuePtr(*argv);
	}
	if (2 <= argc) {
	    Check_Type(argv[1], T_STRING);
	    filename = StringValuePtr(argv[1]);
	}
    }
    if (0 != (leaf = get_doc_leaf(doc, path))) {
	char	*json;
	VALUE	rjson;

	if (0 == filename) {
	    json = oj_write_leaf_to_str(leaf, &oj_default_options);
	    rjson = rb_str_new2(json);
	    xfree(json);
	} else {
	    oj_write_leaf_to_file(leaf, filename, &oj_default_options);
	    rjson = Qnil;
	}
	return rjson;
    }
    return Qnil;
}

/* call-seq: size() => Fixnum
 *
 * Returns the number of nodes in the JSON document where a node is any one of
 * the basic JSON components.
 * @return Returns the size of the JSON document.
 * @example
 *   Oj::Doc.open('[1,2,3]') { |doc| doc.size() }  #=> 4
 */
static VALUE
doc_size(VALUE self) {
    return ULONG2NUM(((Doc)DATA_PTR(self))->size);
}

/* call-seq: close() => nil
 *
 * Closes an open document. No further calls to the document will be valid
 * after closing.
 * @example
 *   doc = Oj::Doc.open('[1,2,3]')
 *   doc.size()  #=> 4
 *   doc.close()
 */
static VALUE
doc_close(VALUE self) {
    Doc		doc = self_doc(self);

    DATA_PTR(doc->self) = 0;
    if (0 != doc) {
	xfree(doc->json);
	doc_free(doc);
    }
    return Qnil;
}

/* Document-class: Oj::Doc
 *
 * The Doc class is used to parse and navigate a JSON document. The model it
 * employs is that of a document that while open can be navigated and values
 * extracted. Once the document is closed the document can not longer be
 * accessed. This allows the parsing and data extraction to be extremely fast
 * compared to other JSON parses.
 * 
 * An Oj::Doc class is not created directly but the _open()_ class method is
 * used to open a document and the yield parameter to the block of the #open()
 * call is the Doc instance. The Doc instance can be moved across, up, and
 * down the JSON document. At each element the data associated with the
 * element can be extracted. It is also possible to just provide a path to the
 * data to be extracted and retrieve the data in that manner.
 * 
 * For many of the methods a path is used to describe the location of an
 * element. Paths follow a subset of the XPath syntax. The slash ('/')
 * character is the separator. Each step in the path identifies the next
 * branch to take through the document. A JSON object will expect a key string
 * while an array will expect a positive index. A .. step indicates a move up
 * the JSON document.
 * 
 * @example
 *   json = %{[
 *     {
 *       "one"   : 1,
 *       "two"   : 2
 *     },
 *     {
 *       "three" : 3,
 *       "four"  : 4
 *     }
 *   ]}
 *   # move and get value
 *   Oj::Doc.open(json) do |doc|
 *     doc.move('/1/two')  
 *     # doc location is now at the 'two' element of the hash that is the first element of the array.
 *     doc.fetch()
 *   end
 *   #=> 2
 *   
 *   # Now try again using a path to Oj::Doc.fetch() directly and not using a block.
 *   doc = Oj::Doc.open(json)
 *   doc.fetch('/2/three')  #=> 3
 *   doc.close()
 */
void
oj_init_doc() {
    oj_doc_class = rb_define_class_under(Oj, "Doc", rb_cObject);
    rb_define_singleton_method(oj_doc_class, "open", doc_open, 1);
    rb_define_singleton_method(oj_doc_class, "open_file", doc_open_file, 1);
    rb_define_singleton_method(oj_doc_class, "parse", doc_open, 1);
    rb_define_method(oj_doc_class, "where?", doc_where, 0);
    rb_define_method(oj_doc_class, "local_key", doc_local_key, 0);
    rb_define_method(oj_doc_class, "home", doc_home, 0);
    rb_define_method(oj_doc_class, "type", doc_type, -1);
    rb_define_method(oj_doc_class, "fetch", doc_fetch, -1);
    rb_define_method(oj_doc_class, "each_leaf", doc_each_leaf, -1);
    rb_define_method(oj_doc_class, "move", doc_move, 1);
    rb_define_method(oj_doc_class, "each_child", doc_each_child, -1);
    rb_define_method(oj_doc_class, "each_value", doc_each_value, -1);
    rb_define_method(oj_doc_class, "dump", doc_dump, -1);
    rb_define_method(oj_doc_class, "size", doc_size, 0);
    rb_define_method(oj_doc_class, "close", doc_close, 0);
}
