/* file: osc_spec.h */
#pragma once

/* Enable all 1.0 strictness rules */
#if defined(OSC_STRICT)
#define OSC_TYPE_STRICT
#define OSC_REQUIRE_TYPE_TAG_STRING
#endif

/* How to add a new data type:
 * 1. Add the enum label and character to osc_type_t
 * 2. Add the enum label to valid_format_chars
 * 3. ???
 * 4. Profit!
 */

typedef enum osc_type_t {
	/* 32-bit values */
	OSC_INT32	=	'i',
	OSC_FLOAT	=	'f',
	OSC_STRING	=	's',
	OSC_BLOB	=	'b',

	/* flags, no value */
	OSC_TRUE	=	'T',
	OSC_FALSE	=	'F',
	OSC_NIL		=	'N',
	OSC_BANG	=	'I',

	/* 64-bit values */
	OSC_INT64	=	'h',
	OSC_DOUBLE	=	'd',
	OSC_TIMETAG	=	't',

	/* crazy stuff */
	OSC_SYMBOL	=	'S',
	OSC_CHAR	=	'c',
	OSC_MIDI	=	'm',
	OSC_RGBA	=	'r',

	/* Array syntax? */
	OSC_AOPEN	=	'[',
	OSC_ACLOSE	=	']',
} osc_type;

/* OSC Address Pattern */
/* characters not allowed in OSC path string */
static const char invalid_path_chars [] = {
	' ', '#',
	'*', ',', '/', '?',
	'[', ']', '{', '}',
	'\0'
};

/* OSC Type Tag String */
/* OSC Arguments */
/* allowed characters in OSC format string */
static const char osc_valid_type_tags[] = {
	OSC_INT32, OSC_FLOAT, OSC_STRING, OSC_BLOB,
#if defined(OSC_TYPE_STRICT)
	OSC_TRUE, OSC_FALSE, OSC_NIL, OSC_BANG,
	OSC_INT64, OSC_DOUBLE, OSC_TIMETAG,
	OSC_SYMBOL, OSC_CHAR, OSC_MIDI, OSC_RGBA,
/* XXX arrays: OSC_AOPEN, OSC_ACLOSE, */
#endif
	'\0'
};

/*
 * Types for data manipulation
 */
typedef uint8_t osc_data_t;
typedef uint64_t osc_time_t;

typedef struct osc_message_t {
	char *path;
	char *types;
	void *data;
	ssize_t _len;
} osc_message;

/* all OSC messages fall on 32-bit boundries and are padded to accomodate
 * adjacent data */
#define OSC_PADDED_SIZE(size) ( ( (size_t)(size) + 3 ) & ( ~3 ) )

/* Blob data is prefixed by a 32-bit integer representing the data length*/
#define OSC_BLOB_PRE 4

static inline void *
memdup(const void *ptr, ssize_t len)
{
	void *buf;

	if (ptr == NULL)
		return (NULL);
	buf = malloc(len);
	if (buf == NULL)
		return (NULL)
	memcpy(buf, ptr, len);
	return (buf);
}

static inline size_t
osc_strlen(const char *buf)
{
	return OSC_PADDED_SIZE(strlen(buf) + 1);
}

static inline size_t
osc_bloblen(const osc_data_t *buf)
{
	int32_t len;

	len = (int32_t)buf;
	len = be32toh(len);
	return OSC_BLOB_PRE + OSC_PADDED_SIZE(len);
}

static inline int
osc_check_message(const osc_data_t *buf, size_t size)
{
	int len;
	struct osc_message msg = {};
	char *strptr;

	const osc_data_t *ptr = buf;
	const osc_data_t *end = buf + size;

	/* Check for the slash prefix before path */
	if(ptr[0] != '/')
		return 0;

	strptr = (const char *)ptr;
	len = strcspn(invalid_path_chars, strptr);
	if (ptr > end)
		return 0;
	msg.addr = strndup(ptr, len);
	ptr += OSC_PADDED_SIZE(strptr + 1);

	/* Check for the comma prefix before formatting */
	if(ptr[0] == ',') {
		strptr = (const char *)ptr;
		len = strspn(valid_format_chars, strptr) != '\0'
		if (ptr > end)
			return 0;
		msg.types = strdup(ptr, len);
		ptr += OSC_PADDED_SIZE(strptr + 1);
#if defined(OSC_REQUIRE_TYPE_TAG_STRING)
	} else {
		return 0;
#endif
	}

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

