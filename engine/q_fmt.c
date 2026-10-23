/*
 * q1psx — snprintf for the screen's text (engine/q_fmt.h).
 */

#include <stdarg.h>
#include <stdint.h>

#include "q_fmt.h"

int q_snprintf(char *buf, int size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = 0;                                   /* the whole text's length so far */
	for (; *fmt; fmt++) {
		char tmp[12];
		const char *s = tmp;
		int len = 1, left = 0, zero = 0, width = 0, sign = 0;
		if (*fmt != '%') {
			tmp[0] = *fmt;
		} else {
			if (*++fmt == '-') {
				left = 1;
				fmt++;
			}
			if (*fmt == '0') {
				zero = 1;
				fmt++;
			}
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + *fmt++ - '0';
			switch (*fmt) {
			case 'd':
			case 'u':
			case 'x': {
				uint32_t v = va_arg(ap, uint32_t), base = *fmt == 'x' ? 16 : 10;
				if (*fmt == 'd' && (int32_t) v < 0) {
					sign = 1;
					v = -v;
				}
				char *p = tmp + sizeof tmp;
				do {
					*--p = "0123456789abcdef"[v % base];
					v /= base;
				} while (v);
				s   = p;
				len = (int) (tmp + sizeof tmp - p);
				break;
			}
			case 's':
				s = va_arg(ap, const char *);
				for (len = 0; s[len]; len++)
					;
				break;
			case 'c':
				tmp[0] = (char) va_arg(ap, int);
				break;
			default:                             /* %%, and what is not a conversion */
				tmp[0] = *fmt;
				if (!*fmt) {                     /* (a '%' ends the format) */
					fmt--;
					len = 0;
				}
				break;
			}
		}
		int pad = width - len - sign;
		if (zero && sign) {                      /* the sign before the zeros */
			if (n < size - 1)
				buf[n] = '-';
			n++;
			sign = 0;
		}
		for (; !left && pad > 0; pad--, n++)
			if (n < size - 1)
				buf[n] = zero ? '0' : ' ';
		if (sign) {
			if (n < size - 1)
				buf[n] = '-';
			n++;
		}
		for (int i = 0; i < len; i++, n++)
			if (n < size - 1)
				buf[n] = s[i];
		for (; pad > 0; pad--, n++)
			if (n < size - 1)
				buf[n] = ' ';
	}
	if (size > 0)
		buf[n < size ? n : size - 1] = 0;
	va_end(ap);
	return n;
}
