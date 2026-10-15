#include <limits.h>
#include <string.h>

#include "json.h"

/* 객체와 배열이 이보다 깊으면 받지 않는다. JWT 와 JWKS 는 셋을 넘지 않는다. 재귀로
 * 내려가므로 깊이를 막지 않으면 "[[[[..." 한 줄로 스택을 다 쓸 수 있다. */
#define JSON_MAX_DEPTH 16

typedef struct parser {
	const char *s;
	int len;
	int pos;
	json_tok_t *t;
	int max;
	int n;
} parser_t;

static int parse_value(parser_t *p, int depth);

static void skip_ws(parser_t *p)
{
	while (p->pos < p->len) {
		char c = p->s[p->pos];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			break;
		}
		p->pos++;
	}
}

static int peek(const parser_t *p)
{
	return p->pos < p->len ? (unsigned char)p->s[p->pos] : -1;
}

static int new_tok(parser_t *p, uint8_t type)
{
	json_tok_t *t;

	if (p->n >= p->max) {
		return -1;
	}
	t = &p->t[p->n];
	t->type = type;
	t->start = p->pos;
	t->end = p->pos;
	t->size = 0;
	t->next = 0;

	return p->n++;
}

static int is_hex(int c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

/* 여는 따옴표에서 시작한다. 토큰은 내용만(따옴표 안쪽) 가리킨다. */
static int parse_string(parser_t *p)
{
	int start = ++p->pos;
	int idx, c, k;

	while (p->pos < p->len) {
		c = (unsigned char)p->s[p->pos];

		if (c == '"') {
			idx = new_tok(p, JSON_STRING);
			if (idx < 0) {
				return -1;
			}
			p->t[idx].start = start;
			p->t[idx].end = p->pos;
			p->pos++;
			p->t[idx].next = p->n;
			return idx;
		}
		if (c < 0x20) {
			return -1;
		}
		if (c == '\\') {
			if (++p->pos >= p->len) {
				return -1;
			}
			c = (unsigned char)p->s[p->pos];
			if (c == 'u') {
				for (k = 1; k <= 4; k++) {
					if (p->pos + k >= p->len || !is_hex((unsigned char)p->s[p->pos + k])) {
						return -1;
					}
				}
				p->pos += 4;
			}
			else if (c == 0 || strchr("\"\\/bfnrt", c) == NULL) {
				return -1;
			}
		}
		p->pos++;
	}

	return -1;
}

/* 숫자 (RFC 8259 의 문법 그대로) 또는 true / false / null. */
static int parse_primitive(parser_t *p)
{
	static const char *const words[] = { "true", "false", "null" };
	int start = p->pos;
	size_t wl;
	int idx, i;

	for (i = 0; i < 3; i++) {
		wl = strlen(words[i]);
		if ((size_t)(p->len - p->pos) >= wl && memcmp(p->s + p->pos, words[i], wl) == 0) {
			p->pos += (int)wl;
			goto done;
		}
	}

	if (peek(p) == '-') {
		p->pos++;
	}
	if (peek(p) == '0') {
		p->pos++;
	}
	else if (is_digit(peek(p))) {
		while (is_digit(peek(p))) p->pos++;
	}
	else {
		return -1;
	}
	if (peek(p) == '.') {
		p->pos++;
		if (!is_digit(peek(p))) return -1;
		while (is_digit(peek(p))) p->pos++;
	}
	if (peek(p) == 'e' || peek(p) == 'E') {
		p->pos++;
		if (peek(p) == '+' || peek(p) == '-') p->pos++;
		if (!is_digit(peek(p))) return -1;
		while (is_digit(peek(p))) p->pos++;
	}

done:
	idx = new_tok(p, JSON_PRIMITIVE);
	if (idx < 0) {
		return -1;
	}
	p->t[idx].start = start;
	p->t[idx].end = p->pos;
	p->t[idx].next = p->n;

	return idx;
}

static int raw_eq(const parser_t *p, int a, int b)
{
	const json_tok_t *x = &p->t[a], *y = &p->t[b];

	return x->end - x->start == y->end - y->start &&
		memcmp(p->s + x->start, p->s + y->start, (size_t)(x->end - x->start)) == 0;
}

static int parse_object(parser_t *p, int depth)
{
	int idx = new_tok(p, JSON_OBJECT);
	int key, val, k, prev;

	if (idx < 0) {
		return -1;
	}
	p->pos++;
	skip_ws(p);

	if (peek(p) == '}') {
		p->pos++;
		goto done;
	}

	for (;;) {
		if (peek(p) != '"') {
			return -1;
		}
		key = parse_string(p);
		if (key < 0) {
			return -1;
		}
		/* 키에는 이스케이프를 받지 않는다. 그래야 원문 비교(json_get)가 정확하다 —
		 * "aud" 를 다른 구현은 "aud" 로 읽는데 우리는 못 찾는 일이 없다. */
		if (memchr(p->s + p->t[key].start, '\\', (size_t)(p->t[key].end - p->t[key].start)) != NULL) {
			return -1;
		}
		for (k = 0, prev = idx + 1; k < p->t[idx].size; k++, prev = p->t[prev + 1].next) {
			if (raw_eq(p, prev, key)) {
				return -1;
			}
		}

		skip_ws(p);
		if (peek(p) != ':') {
			return -1;
		}
		p->pos++;

		val = parse_value(p, depth + 1);
		if (val < 0) {
			return -1;
		}
		p->t[idx].size++;

		skip_ws(p);
		if (peek(p) == ',') {
			p->pos++;
			skip_ws(p);
			continue;
		}
		if (peek(p) == '}') {
			p->pos++;
			break;
		}
		return -1;
	}

done:
	p->t[idx].end = p->pos;
	p->t[idx].next = p->n;

	return idx;
}

static int parse_array(parser_t *p, int depth)
{
	int idx = new_tok(p, JSON_ARRAY);

	if (idx < 0) {
		return -1;
	}
	p->pos++;
	skip_ws(p);

	if (peek(p) == ']') {
		p->pos++;
		goto done;
	}

	for (;;) {
		if (parse_value(p, depth + 1) < 0) {
			return -1;
		}
		p->t[idx].size++;

		skip_ws(p);
		if (peek(p) == ',') {
			p->pos++;
			continue;
		}
		if (peek(p) == ']') {
			p->pos++;
			break;
		}
		return -1;
	}

done:
	p->t[idx].end = p->pos;
	p->t[idx].next = p->n;

	return idx;
}

static int parse_value(parser_t *p, int depth)
{
	if (depth > JSON_MAX_DEPTH) {
		return -1;
	}

	skip_ws(p);
	switch (peek(p)) {
		case '{': return parse_object(p, depth);
		case '[': return parse_array(p, depth);
		case '"': return parse_string(p);
		case -1:  return -1;
		default:  return parse_primitive(p);
	}
}

int json_parse(const char *s, size_t len, json_tok_t *t, int max)
{
	parser_t p;

	if (len > INT_MAX / 2) {
		return -1;
	}

	p.s = s;
	p.len = (int)len;
	p.pos = 0;
	p.t = t;
	p.max = max;
	p.n = 0;

	if (parse_value(&p, 0) < 0) {
		return -1;
	}
	skip_ws(&p);
	if (p.pos != p.len) {
		return -1;
	}

	return p.n;
}

int json_eq(const char *s, const json_tok_t *t, int i, const char *want)
{
	size_t wl = strlen(want);

	return t[i].type == JSON_STRING && (size_t)(t[i].end - t[i].start) == wl &&
		memcmp(s + t[i].start, want, wl) == 0;
}

int json_get(const char *s, const json_tok_t *t, int obj, const char *key)
{
	int i, k;

	if (t[obj].type != JSON_OBJECT) {
		return -1;
	}

	for (k = 0, i = obj + 1; k < t[obj].size; k++, i = t[i + 1].next) {
		if (json_eq(s, t, i, key)) {
			return i + 1;
		}
	}

	return -1;
}

static int hex4(const char *h)
{
	int v = 0, i, c;

	for (i = 0; i < 4; i++) {
		c = (unsigned char)h[i];
		v = v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
	}

	return v;
}

/* 코드 포인트 하나를 UTF-8 로 쓴다. 쓴 바이트 수, 자리가 모자라면 -1. */
static int put_utf8(char *out, size_t cap, size_t at, unsigned long cp)
{
	unsigned char b[4];
	int n, i;

	if (cp < 0x80) {
		b[0] = (unsigned char)cp; n = 1;
	}
	else if (cp < 0x800) {
		b[0] = (unsigned char)(0xC0 | (cp >> 6));
		b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
	}
	else if (cp < 0x10000) {
		b[0] = (unsigned char)(0xE0 | (cp >> 12));
		b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
	}
	else {
		b[0] = (unsigned char)(0xF0 | (cp >> 18));
		b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
		b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
	}

	if (at + (size_t)n >= cap) {
		return -1;
	}
	for (i = 0; i < n; i++) {
		out[at + (size_t)i] = (char)b[i];
	}

	return n;
}

int json_string(const char *s, const json_tok_t *t, int i, char *out, size_t cap)
{
	size_t o = 0;
	unsigned long cp, lo;
	int k, w;
	char c;

	if (t[i].type != JSON_STRING || cap == 0) {
		return -1;
	}

	/* 파싱할 때 이스케이프의 모양은 이미 확인했다. */
	for (k = t[i].start; k < t[i].end; k++) {
		c = s[k];
		if (c != '\\') {
			if (o + 1 >= cap) {
				return -1;
			}
			out[o++] = c;
			continue;
		}

		c = s[++k];
		if (c != 'u') {
			if (o + 1 >= cap) {
				return -1;
			}
			switch (c) {
				case 'b': out[o++] = '\b'; break;
				case 'f': out[o++] = '\f'; break;
				case 'n': out[o++] = '\n'; break;
				case 'r': out[o++] = '\r'; break;
				case 't': out[o++] = '\t'; break;
				default:  out[o++] = c; break;	/* " \ / */
			}
			continue;
		}

		cp = (unsigned long)hex4(s + k + 1);
		k += 4;
		/* UTF-16 대리 쌍. 짝이 없는 반쪽은 받지 않는다. */
		if (cp >= 0xD800 && cp <= 0xDBFF) {
			if (k + 6 >= t[i].end || s[k + 1] != '\\' || s[k + 2] != 'u') {
				return -1;
			}
			lo = (unsigned long)hex4(s + k + 3);
			if (lo < 0xDC00 || lo > 0xDFFF) {
				return -1;
			}
			cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			k += 6;
		}
		else if (cp >= 0xDC00 && cp <= 0xDFFF) {
			return -1;
		}
		if (cp == 0) {
			return -1;
		}

		w = put_utf8(out, cap, o, cp);
		if (w < 0) {
			return -1;
		}
		o += (size_t)w;
	}

	out[o] = '\0';

	return (int)o;
}

int json_int64(const char *s, const json_tok_t *t, int i, int64_t *out)
{
	int k = t[i].start;
	int neg = 0;
	int64_t v = 0;

	if (t[i].type != JSON_PRIMITIVE) {
		return -1;
	}
	if (s[k] == '-') {
		neg = 1;
		k++;
	}
	if (k >= t[i].end || !is_digit((unsigned char)s[k])) {
		return -1;	/* true / false / null */
	}

	for (; k < t[i].end && is_digit((unsigned char)s[k]); k++) {
		if (v > (INT64_MAX - (s[k] - '0')) / 10) {
			return -1;
		}
		v = v * 10 + (s[k] - '0');
	}
	/* 1.7e9 를 1 로 읽으면 안 되므로 지수가 붙은 숫자는 받지 않는다. 소수부는 버린다. */
	for (; k < t[i].end; k++) {
		if (s[k] == 'e' || s[k] == 'E') {
			return -1;
		}
	}

	*out = neg ? -v : v;

	return 0;
}
