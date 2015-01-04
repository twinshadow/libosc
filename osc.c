/*
 * Copyright (c) 2015 Hanspeter Portner (dev@open-music-kontrollers.ch)
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the Artistic License 2.0 as published by
 * The Perl Foundation.
 *
 * This source is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * Artistic License 2.0 for more details.
 *
 * You should have received a copy of the Artistic License 2.0
 * along the source as a COPYING file. If not, obtain it from
 * http://www.perlfoundation.org/artistic_license_2_0.
 */

#include <osc.h>

#include <stdarg.h>
#include <ctype.h>

// characters not allowed in OSC path string
static const char invalid_path_chars [] = {
	' ', '#',
	'\0'
};

// allowed characters in OSC format string
static const char valid_format_chars [] = {
	OSC_INT32, OSC_FLOAT, OSC_STRING, OSC_BLOB,
	OSC_TRUE, OSC_FALSE, OSC_NIL, OSC_BANG,
	OSC_INT64, OSC_DOUBLE, OSC_TIMETAG,
	OSC_SYMBOL, OSC_CHAR, OSC_MIDI,
	'\0'
};

// check for valid path string
int
osc_check_path(const char *path)
{
	const char *ptr;

	if(path[0] != '/')
		return 0;

	for(ptr=path+1; *ptr!='\0'; ptr++)
		if( (isprint(*ptr) == 0) || (strchr(invalid_path_chars, *ptr) != NULL) )
			return 0;

	return 1;
}

// check for valid format string 
int
osc_check_fmt(const char *format, int offset)
{
	const char *ptr;

	if(offset)
		if(format[0] != ',')
			return 0;

	for(ptr=format+offset; *ptr!='\0'; ptr++)
		if(strchr(valid_format_chars, *ptr) == NULL)
			return 0;

	return 1;
}

int
osc_match_method(osc_method_t *methods, const char *path, const char *fmt)
{
	osc_method_t *meth;
	for(meth=methods; meth->cb; meth++)
		if( (!meth->path || !strcmp(meth->path, path)) && (!meth->fmt || !strcmp(meth->fmt, fmt+1)) )
			return 1;
	return 0;
}

static void
_osc_method_dispatch_message(uint64_t time, osc_data_t *buf, size_t size, osc_method_t *methods, void *dat)
{
	osc_data_t *ptr = buf;
	osc_data_t *end = buf + size;

	const char *path;
	const char *fmt;

	ptr = osc_get_path(ptr, &path);
	ptr = osc_get_fmt(ptr, &fmt);

	osc_method_t *meth;
	for(meth=methods; meth->cb; meth++)
		if( (!meth->path || !strcmp(meth->path, path)) && (!meth->fmt || !strcmp(meth->fmt, fmt+1)) )
			if(meth->cb(time, path, fmt+1, ptr, size-(ptr-buf), dat))
				break;
}

static void
_osc_method_dispatch_bundle(uint64_t time, osc_data_t *buf, size_t size, osc_method_t *methods, osc_bundle_in_cb_t bundle_in, osc_bundle_out_cb_t bundle_out, void *dat)
{
	osc_data_t *ptr = buf;
	osc_data_t *end = buf + size;

	if(bundle_in)
		bundle_in(time, dat);

	ptr += 16; // skip bundle header

	while(ptr < end)
	{
		int32_t len = be32toh(*((int32_t *)ptr));
		ptr += sizeof(int32_t);
		switch(*ptr)
		{
			case '#':
				_osc_method_dispatch_bundle(time, ptr, len, methods, bundle_in, bundle_out, dat);
				break;
			case '/':
				_osc_method_dispatch_message(time, ptr, len, methods, dat);
				break;
		}
		ptr += len;
	}

	if(bundle_out)
		bundle_out(time, dat);
}

void
osc_dispatch_method(uint64_t time, osc_data_t *buf, size_t size, osc_method_t *methods, osc_bundle_in_cb_t bundle_in, osc_bundle_out_cb_t bundle_out, void *dat)
{
	switch(*buf)
	{
		case '#':
			_osc_method_dispatch_bundle(time, buf, size, methods, bundle_in, bundle_out, dat);
			break;
		case '/':
			_osc_method_dispatch_message(time, buf, size, methods, dat);
			break;
	}
}

// extract nested bundles with non-matching timestamps
static int
_unroll_partial(osc_data_t *buf, size_t size, osc_unroll_inject_t *inject, void *dat)
{
	if(strncmp((char *)buf, "#bundle", 8)) // bundle header valid?
		return 0;

	osc_data_t *end = buf + size;
	osc_data_t *ptr = buf;

	uint64_t timetag = be64toh(*(uint64_t *)(buf + 8));
	inject->stamp(timetag, dat);

	int has_messages = 0;
	int has_nested_bundles = 0;

	ptr = buf + 16; // skip bundle header
	while(ptr < end)
	{
		int32_t *size = (int32_t *)ptr;
		int32_t hsize = be32toh(*size);
		ptr += sizeof(int32_t);

		char c = *(char *)ptr;
		switch(c)
		{
			case '#':
				has_nested_bundles = 1;
				if(!_unroll_partial(ptr, hsize, inject, dat))
					return 0;
				break;
			case '/':
				has_messages = 1;
				// ignore for now
				break;
			default:
				return 0;
		}

		ptr += hsize;
	}

	if(!has_nested_bundles)
	{
		if(has_messages)
			inject->bundle(buf, size, dat);
		return 1;
	}

	if(!has_messages)
		return 1; // discard empty bundles

	// repack bundle with messages only, ignoring nested bundles
	ptr = buf + 16; // skip bundle header
	osc_data_t *dst = ptr;
	while(ptr < end)
	{
		int32_t *size = (int32_t *)ptr;
		int32_t hsize = be32toh(*size);
		ptr += sizeof(int32_t);

		char *c = (char *)ptr;
		if(*c == '/')
		{
			memmove(dst, ptr - sizeof(int32_t), sizeof(int32_t) + hsize);
			dst += sizeof(int32_t) + hsize;
		}

		ptr += hsize;
	}

	size_t nlen = dst - buf; 
	inject->bundle(buf, nlen, dat);

	return 1;
}

// fully unroll bundle into single messages
static int
_unroll_full(osc_data_t *buf, size_t size, osc_unroll_inject_t *inject, void *dat)
{
	if(strncmp((char *)buf, "#bundle", 8)) // bundle header valid?
		return 0;

	osc_data_t *end = buf + size;
	osc_data_t *ptr = buf;

	uint64_t timetag = be64toh(*(uint64_t *)(buf + 8));
	inject->stamp(timetag, dat);

	int has_nested_bundles = 0;

	ptr = buf + 16; // skip bundle header
	while(ptr < end)
	{
		int32_t *size = (int32_t *)ptr;
		int32_t hsize = be32toh(*size);
		ptr += sizeof(int32_t);

		char c = *(char *)ptr;
		switch(c)
		{
			case '#':
				has_nested_bundles = 1;
				// ignore for now, messages are handled first
				break;
			case '/':
				inject->message(ptr, hsize, dat);
				break;
			default:
				return 0;
		}

		ptr += hsize;
	}

	if(!has_nested_bundles)
		return 1;

	ptr = buf + 16; // skip bundle header
	while(ptr < end)
	{
		int32_t *size = (int32_t *)ptr;
		int32_t hsize = be32toh(*size);
		ptr += sizeof(int32_t);

		char *c = (char *)ptr;
		if(*c == '#')
			if(!_unroll_full(ptr, hsize, inject, dat))
				return 0;

		ptr += hsize;
	}

	return 1;
}

int
osc_unroll_packet(osc_data_t *buf, size_t size, osc_unroll_mode_t mode, osc_unroll_inject_t *inject, void *dat)
{
	char c = *(char *)buf;
	switch(c)
	{
		case '#':
			switch(mode)
			{
				case OSC_UNROLL_MODE_NONE:
					inject->bundle(buf, size, dat);
					break;
				case OSC_UNROLL_MODE_PARTIAL:
					if(!_unroll_partial(buf, size, inject, dat))
						return 0;
					break;
				case OSC_UNROLL_MODE_FULL:
					if(!_unroll_full(buf, size, inject, dat))
						return 0;
					break;
			}
			break;
		case '/':
			inject->message(buf, size, dat);
			break;
		default:
			return 0;
	}

	return 1;
}

int
osc_check_message(osc_data_t *buf, size_t size)
{
	osc_data_t *ptr = buf;
	osc_data_t *end = buf + size;

	const char *path;
	const char *fmt;

	ptr = osc_get_path(ptr, &path);
	if( (ptr > end) || !osc_check_path(path) )
		return 0;

	ptr = osc_get_fmt(ptr, &fmt);
	if( (ptr > end) || !osc_check_fmt(fmt, 1) )
		return 0;

	const char *type;
	for(type=fmt+1; (*type!='\0') && (ptr <= end); type++)
	{
		switch(*type)
		{
			case OSC_INT32:
			case OSC_FLOAT:
			case OSC_MIDI:
			case OSC_CHAR:
				ptr += 4;
				break;

			case OSC_STRING:
			case OSC_SYMBOL:
				ptr += osc_strlen((const char *)ptr);
				break;

			case OSC_BLOB:
				ptr += osc_bloblen(ptr);
				break;

			case OSC_INT64:
			case OSC_DOUBLE:
			case OSC_TIMETAG:
				ptr += 8;
				break;

			case OSC_TRUE:
			case OSC_FALSE:
			case OSC_NIL:
			case OSC_BANG:
				break;
		}
	}

	return ptr == end;
}

int
osc_check_bundle(osc_data_t *buf, size_t size)
{
	osc_data_t *ptr = buf;
	osc_data_t *end = buf + size;
	
	if(strncmp((char *)ptr, "#bundle", 8)) // bundle header valid?
		return 0;
	ptr += 16; // skip bundle header

	while(ptr < end)
	{
		int32_t *len = (int32_t *)ptr;
		int32_t hlen = be32toh(*len);
		ptr += sizeof(int32_t);

		switch(*ptr)
		{
			case '#':
				if(!osc_check_bundle(ptr, hlen))
					return 0;
				break;
			case '/':
				if(!osc_check_message(ptr, hlen))
					return 0;
				break;
			default:
				return 0;
		}
		ptr += hlen;
	}

	return ptr == end;
}

int
osc_check_packet(osc_data_t *buf, size_t size)
{
	osc_data_t *ptr = buf;
	
	switch(*ptr)
	{
		case '#':
			if(!osc_check_bundle(ptr, size))
				return 0;
			break;
		case '/':
			if(!osc_check_message(ptr, size))
				return 0;
			break;
		default:
			return 0;
	}

	return 1;
}

osc_data_t *
osc_skip(osc_type_t type, osc_data_t *buf)
{
	switch(type)
	{
		case OSC_INT32:
		case OSC_FLOAT:
		case OSC_MIDI:
		case OSC_CHAR:
			return buf + 4;

		case OSC_STRING:
		case OSC_SYMBOL:
			return buf + osc_strlen((const char *)buf);

		case OSC_BLOB:
			return buf + osc_bloblen(buf);

		case OSC_INT64:
		case OSC_DOUBLE:
		case OSC_TIMETAG:
			return buf + 8;

		case OSC_TRUE:
		case OSC_FALSE:
		case OSC_NIL:
		case OSC_BANG:
			return buf;

		default:
			return NULL;
	}
}

osc_data_t *
osc_get(osc_type_t type, osc_data_t *buf, osc_argument_t *arg)
{
	switch(type)
	{
		case OSC_INT32:
			return osc_get_int32(buf, &arg->i);
		case OSC_FLOAT:
			return osc_get_float(buf, &arg->f);
		case OSC_STRING:
			return osc_get_string(buf, &arg->s);
		case OSC_BLOB:
			return osc_get_blob(buf, &arg->b);

		case OSC_INT64:
			return osc_get_int64(buf, &arg->h);
		case OSC_DOUBLE:
			return osc_get_double(buf, &arg->d);
		case OSC_TIMETAG:
			return osc_get_timetag(buf, &arg->t);

		case OSC_TRUE:
		case OSC_FALSE:
		case OSC_NIL:
		case OSC_BANG:
			return buf;

		case OSC_SYMBOL:
			return osc_get_symbol(buf, &arg->S);
		case OSC_CHAR:
			return osc_get_char(buf, &arg->c);
		case OSC_MIDI:
			return osc_get_midi(buf, &arg->m);

		default:
			return NULL;
	}
}

osc_data_t *
osc_get_vararg(osc_data_t *buf, const char **path, const char **fmt, ...)
{
	osc_data_t *ptr = buf;

  va_list args;
  va_start (args, fmt);

	ptr = osc_get_path(ptr, path);
	ptr = osc_get_fmt(ptr, fmt);

  const char *type;
  for(type=*fmt; *type != '\0'; type++)
		switch(*type)
		{
			case OSC_INT32:
				ptr = osc_get_int32(ptr, va_arg(args, int32_t *));
				break;
			case OSC_FLOAT:
				ptr = osc_get_float(ptr, va_arg(args, float *));
				break;
			case OSC_STRING:
				ptr = osc_get_string(ptr, va_arg(args, const char **));
				break;
			case OSC_BLOB:
				ptr = osc_get_blob(ptr, va_arg(args, osc_blob_t *));
				break;

			case OSC_INT64:
				ptr = osc_get_int64(ptr, va_arg(args, int64_t *));
				break;
			case OSC_DOUBLE:
				ptr = osc_get_double(ptr, va_arg(args, double *));
				break;
			case OSC_TIMETAG:
				ptr = osc_get_timetag(ptr, va_arg(args, uint64_t *));
				break;

			case OSC_TRUE:
			case OSC_FALSE:
			case OSC_NIL:
			case OSC_BANG:
				break;

			case OSC_SYMBOL:
				ptr = osc_get_symbol(ptr, va_arg(args, const char **));
				break;
			case OSC_CHAR:
				ptr = osc_get_char(ptr, va_arg(args, char *));
				break;
			case OSC_MIDI:
				ptr = osc_get_midi(ptr, va_arg(args, uint8_t **));
				break;

			default:
				ptr = NULL;
				break;
		}

  va_end(args);

	return ptr;
}

osc_data_t *
osc_set(osc_data_t *buf, const osc_data_t *end, osc_type_t type, osc_argument_t *arg)
{
	switch(type)
	{
		case OSC_INT32:
			return osc_set_int32(buf, end, arg->i);
		case OSC_FLOAT:
			return osc_set_float(buf, end, arg->f);
		case OSC_STRING:
			return osc_set_string(buf, end, arg->s);
		case OSC_BLOB:
			return osc_set_blob(buf, end, arg->b.size, arg->b.payload);

		case OSC_INT64:
			return osc_set_int64(buf, end, arg->h);
		case OSC_DOUBLE:
			return osc_set_double(buf, end, arg->d);
		case OSC_TIMETAG:
			return osc_set_timetag(buf, end, arg->t);

		case OSC_TRUE:
		case OSC_FALSE:
		case OSC_NIL:
		case OSC_BANG:
			return buf;

		case OSC_SYMBOL:
			return osc_set_symbol(buf, end, arg->S);
		case OSC_CHAR:
			return osc_set_char(buf, end, arg->c);
		case OSC_MIDI:
			return osc_set_midi(buf, end, arg->m);

		default:
			return NULL;
	}
}

osc_data_t *
osc_set_varlist(osc_data_t *buf, const osc_data_t *end, const char *path, const char *fmt, va_list args)
{
	osc_data_t *ptr = buf;

	ptr = osc_set_path(ptr, end, path);
	ptr = osc_set_fmt(ptr, end, fmt);

  const char *type;
  for(type=fmt; *type != '\0'; type++)
		switch(*type)
		{
			case OSC_INT32:
				ptr = osc_set_int32(ptr, end, va_arg(args, int32_t));
				break;
			case OSC_FLOAT:
				ptr = osc_set_float(ptr, end, (float)va_arg(args, double));
				break;
			case OSC_STRING:
				ptr = osc_set_string(ptr, end, va_arg(args, char *));
				break;
			case OSC_BLOB:
				ptr = osc_set_blob(ptr, end, va_arg(args, int32_t), va_arg(args, void *));
				break;

			case OSC_INT64:
				ptr = osc_set_int64(ptr, end, va_arg(args, int64_t));
				break;
			case OSC_DOUBLE:
				ptr = osc_set_double(ptr, end, va_arg(args, double));
				break;
			case OSC_TIMETAG:
				ptr = osc_set_timetag(ptr, end, va_arg(args, uint64_t));
				break;

			case OSC_TRUE:
			case OSC_FALSE:
			case OSC_NIL:
			case OSC_BANG:
				break;

			case OSC_SYMBOL:
				ptr = osc_set_symbol(ptr, end, va_arg(args, char *));
				break;
			case OSC_CHAR:
				ptr = osc_set_char(ptr, end, (char)va_arg(args, int));
				break;
			case OSC_MIDI:
				ptr = osc_set_midi(ptr, end, va_arg(args, uint8_t *));
				break;

			default:
				ptr = NULL;
		}

	return ptr;
}

osc_data_t *
osc_set_vararg(osc_data_t *buf, const osc_data_t *end, const char *path, const char *fmt, ...)
{
	osc_data_t *ptr = buf;

  va_list args;
  va_start (args, fmt);

	ptr = osc_set_varlist(ptr, end, path, fmt, args);

  va_end(args);

	return ptr;
}

osc_data_t *
osc_set_bundle_item(osc_data_t *buf, const osc_data_t *end, const char *path, const char *fmt, ...)
{
	osc_data_t *ptr = buf;
	osc_data_t *itm;

  va_list args;
  va_start (args, fmt);

	ptr = osc_start_bundle_item(ptr, end, &itm);
	ptr = osc_set_varlist(ptr, end, path, fmt, args);
	ptr = osc_end_bundle_item(ptr, end, itm);

  va_end(args);

	return ptr;
}
