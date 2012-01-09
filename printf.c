/*
 * This file is part of ltrace.
 * Copyright (C) 2011,2012 Petr Machata, Red Hat Inc.
 * Copyright (C) 1998,2004,2007,2008,2009 Juan Cespedes
 * Copyright (C) 2006 Steve Fink
 * Copyright (C) 2006 Ian Wienand
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <assert.h>
#include <stdlib.h>

#include "printf.h"
#include "type.h"
#include "value.h"
#include "expr.h"
#include "zero.h"
#include "param.h"
#include "lens_default.h"

struct param_enum {
	struct value array;
	int percent;
	size_t *future_length;
	char *format;
	char const *ptr;
	char const *end;
};

static struct param_enum *
param_printf_init(struct value *cb_args, size_t nargs,
		  struct value_dict *arguments)
{
	assert(nargs == 1);

	/* We expect a char array pointer.  */
	if (cb_args->type->type != ARGTYPE_POINTER
	    || cb_args->type->u.ptr_info.info->type != ARGTYPE_ARRAY
	    || (cb_args->type->u.ptr_info.info->u.array_info.elt_type->type
		!= ARGTYPE_CHAR))
		return NULL;

	struct param_enum *self = malloc(sizeof(*self));
	if (self == NULL) {
	fail:
		free(self);
		return NULL;
	}

	if (value_init_deref(&self->array, cb_args) < 0)
		goto fail;

	assert(self->array.type->type == ARGTYPE_ARRAY);

	self->format = (char *)value_get_data(&self->array, arguments);
	if (self->format == NULL)
		goto fail;

	size_t size = value_size(&self->array, arguments);
	if (size == (size_t)-1)
		goto fail;

	self->percent = 0;
	self->ptr = self->format;
	self->end = self->format + size;
	self->future_length = NULL;
	return self;
}

static void
drop_future_length(struct param_enum *self)
{
	if (self->future_length != NULL) {
		free(self->future_length);
		self->future_length = NULL;
	}
}

static int
form_next_param(struct param_enum *self,
		enum arg_type format_type, enum arg_type elt_type,
		unsigned hlf, unsigned lng, char *len_buf, size_t len_buf_len,
		struct arg_type_info *infop)
{
	/* XXX note: Some types are wrong because we lack
	   ARGTYPE_LONGLONG, ARGTYPE_UCHAR and ARGTYPE_SCHAR.  */
	assert(lng <= 2);
	assert(hlf <= 2);
	static enum arg_type ints[] =
		{ ARGTYPE_CHAR, ARGTYPE_SHORT, ARGTYPE_INT,
		  ARGTYPE_LONG, ARGTYPE_ULONG };
	static enum arg_type uints[] =
		{ ARGTYPE_CHAR, ARGTYPE_USHORT, ARGTYPE_UINT,
		  ARGTYPE_ULONG, ARGTYPE_ULONG };

	struct arg_type_info *elt_info = NULL;
	if (format_type == ARGTYPE_ARRAY || format_type == ARGTYPE_POINTER)
		elt_info = type_get_simple(elt_type);
	else if (format_type == ARGTYPE_INT)
		format_type = ints[2 + lng - hlf];
	else if (format_type == ARGTYPE_UINT)
		format_type = uints[2 + lng - hlf];


	if (format_type == ARGTYPE_ARRAY) {
		struct expr_node *node = NULL;
		if (len_buf_len != 0
		    || self->future_length != NULL) {
			struct tmp {
				struct expr_node node;
				struct arg_type_info type;
			};
			struct tmp *len = malloc(sizeof(*len));
			if (len == NULL) {
			fail:
				free(len);
				return -1;
			}

			len->type.type = ARGTYPE_LONG;

			long l;
			if (self->future_length != NULL) {
				l = *self->future_length;
				drop_future_length(self);
			} else {
				l = atol(len_buf);
			}

			expr_init_const_word(&len->node, l, &len->type, 0);

			node = build_zero_w_arg(&len->node, 1);
			if (node == NULL)
				goto fail;

		} else {
			node = expr_node_zero();
		}

		assert(node != NULL);
		type_init_array(infop, elt_info, 0, node, 1);

	} else if (format_type == ARGTYPE_POINTER) {
		type_init_pointer(infop, elt_info, 1);

	} else {
		*infop = *type_get_simple(format_type);
	}

	return 0;
}

static int
param_printf_next(struct param_enum *self, struct arg_type_info *infop,
		  int *insert_stop)
{
	unsigned hlf = 0;
	unsigned lng = 0;
	enum arg_type format_type = ARGTYPE_VOID;
	enum arg_type elt_type = ARGTYPE_VOID;
	char len_buf[25] = {};
	size_t len_buf_len = 0;
	struct lens *lens = NULL;

	for (; self->ptr < self->end; ++self->ptr) {
		if (!self->percent) {
			if (*self->ptr == '%')
				self->percent = 1;
			continue;
		}

		switch (*self->ptr) {
		case '#': case ' ': case '-':
		case '+': case 'I': case '\'':
			/* These are only important for formatting,
			 * not for interpreting the type.  */
			continue;

		case '*':
			/* Length parameter given in the next
			 * argument.  */
			if (self->future_length == NULL)
				/* This should really be an assert,
				 * but we can't just fail on invalid
				 * format string.  */
				self->future_length
					= malloc(sizeof(*self->future_length));

			if (self->future_length != NULL) {
				++self->ptr;
				format_type = ARGTYPE_INT;
				break;
			}

		case '0':
		case '1': case '2': case '3':
		case '4': case '5': case '6':
		case '7': case '8': case '9':
			/* Field length likewise, but we need to parse
			 * this to attach the appropriate string
			 * length expression.  */
			if (len_buf_len < sizeof(len_buf) - 1)
				len_buf[len_buf_len++] = *self->ptr;
			continue;

		case 'h':
			if (hlf < 2)
				hlf++;
			continue;

		case 'l':
			if (lng < 2)
				lng++;
			continue;

		case 'q':
			lng = 2;
			continue;

		case 'L': /* long double */
			lng = 1;
			continue;

		case 'j': /* intmax_t */
			/*   XXX ABI should know */
			lng = 2;
			continue;

		case 't': /* ptrdiff_t */
		case 'Z': case 'z': /* size_t */
			lng = 1; /* XXX ABI should tell */
			continue;

		case 'd':
		case 'i':
			format_type = ARGTYPE_INT;
			self->percent = 0;
			break;

		case 'u':
		case 'o':
		case 'x': case 'X':
			format_type = ARGTYPE_UINT;
			self->percent = 0;
			break;

		case 'e': case 'E':
		case 'f': case 'F':
		case 'g': case 'G':
		case 'a': case 'A':
			format_type = ARGTYPE_DOUBLE;
			self->percent = 0;
			break;

		case 'C': /* like "lc" */
			if (lng == 0)
				lng++;
		case 'c':
			/* XXX "lc" means wchar_t string.  */
			format_type = ARGTYPE_CHAR;
			self->percent = 0;
			break;

		case 'S': /* like "ls" */
			if (lng == 0)
				lng++;
		case 's':
			format_type = ARGTYPE_ARRAY;
			/* XXX "ls" means wchar_t string.  */
			elt_type = ARGTYPE_CHAR;
			self->percent = 0;
			lens = &string_lens;
			break;

		case 'p':
		case 'n': /* int* where to store no. of printed chars.  */
			format_type = ARGTYPE_POINTER;
			elt_type = ARGTYPE_VOID;
			self->percent = 0;
			break;

		case 'm': /* (glibc) print argument of errno */
		case '%':
			lng = 0;
			hlf = 0;
			self->percent = 0;
			continue;

		default:
			continue;
		}

		/* If we got here, the type must have been set.  */
		assert(format_type != ARGTYPE_VOID);

		if (form_next_param(self, format_type, elt_type, hlf, lng,
				    len_buf, len_buf_len, infop) < 0)
			return -1;

		infop->lens = lens;
		infop->own_lens = 0;

		return 0;
	}

	infop->type = ARGTYPE_VOID;
	return 0;
}

static enum param_status
param_printf_stop(struct param_enum *self, struct value *value)
{
	if (self->future_length != NULL
	    && value_extract_word(value, (long *)self->future_length, NULL) < 0)
		drop_future_length(self);

	return PPCB_CONT;
}

static void
param_printf_done(struct param_enum *context)
{
	free(context);
}

void
param_pack_init_printf(struct param *param, struct expr_node *arg, int own_arg)
{
	param_init_pack(param, arg, 1, own_arg,
			&param_printf_init, &param_printf_next,
			&param_printf_stop, &param_printf_done);
}
