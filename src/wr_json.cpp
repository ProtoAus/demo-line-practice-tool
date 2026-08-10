// wr_json.cpp  --  see wr_json.h.

#include "wr_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The cursor
// ---------------------------------------------------------------------------

static void Fail(WrJson *j)
{
    j->bad = true;
    j->p = j->end;
    j->value = NULL;
}

static void SkipWs(WrJson *j)
{
    while (j->p < j->end)
    {
        char c = *j->p;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        j->p++;
    }
}

static char Peek(WrJson *j)
{
    SkipWs(j);
    return (j->p < j->end) ? *j->p : '\0';
}

void WrJsonInit(WrJson *j, const char *text, size_t len)
{
    j->p = text;
    j->end = text ? text + len : NULL;
    j->value = NULL;
    j->depth = 0;
    j->bad = (text == NULL);

    // A UTF-8 BOM. Python's json rejects one outright; we are more forgiving
    // because the only thing that would produce it is a hand-made fixture, and
    // failing on an invisible character is a bad afternoon.
    if (!j->bad && (size_t)(j->end - j->p) >= 3 &&
        (unsigned char)j->p[0] == 0xEF && (unsigned char)j->p[1] == 0xBB &&
        (unsigned char)j->p[2] == 0xBF)
        j->p += 3;

    if (!j->bad)
        j->value = j->p;
}

bool WrJsonFailed(const WrJson *j) { return j->bad; }

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

static int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Read \uXXXX and return the code unit, or -1.
static int Hex4(WrJson *j)
{
    if (j->end - j->p < 4)
        return -1;
    int v = 0;
    for (int i = 0; i < 4; i++)
    {
        int n = HexNibble(j->p[i]);
        if (n < 0)
            return -1;
        v = (v << 4) | n;
    }
    j->p += 4;
    return v;
}

// One appender, for both the escaped path and the raw one.
//
// TRUNCATION IS WHOLE CHARACTERS ONLY, and once it happens nothing more is
// written at all. Both halves of that matter. Half a UTF-8 sequence is a byte
// that is not text, and these strings go into a .tsv the panel reads back and
// into a player's name on screen. And a later, shorter character squeezing in
// after a longer one was rejected would reorder the name.
struct WrStrOut
{
    char *buf;
    int cap;
    int len;
    bool full;
};

static void Append(WrStrOut *o, const char *bytes, int n)
{
    if (!o->buf || o->full)
        return;
    if (o->len + n >= o->cap)
    {
        o->full = true;
        return;
    }
    for (int i = 0; i < n; i++)
        o->buf[o->len++] = bytes[i];
}

static void PutUtf8(unsigned int cp, WrStrOut *o)
{
    char buf[4];
    int n;
    if (cp < 0x80)              { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800)        { buf[0] = (char)(0xC0 | (cp >> 6));
                                  buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000)      { buf[0] = (char)(0xE0 | (cp >> 12));
                                  buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                  buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                        { buf[0] = (char)(0xF0 | (cp >> 18));
                                  buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                  buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                  buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    Append(o, buf, n);
}

// Read a string. `out` may be NULL to skip one. The cursor must be on the '"'.
static bool ReadString(WrJson *j, char *out, int cap)
{
    if (Peek(j) != '"')
        return false;
    j->p++;

    WrStrOut o;
    o.buf = out;
    o.cap = cap;
    o.len = 0;
    o.full = false;

    while (j->p < j->end)
    {
        unsigned char c = (unsigned char)*j->p;
        if (c == '"')
        {
            j->p++;
            if (out)
                out[o.len] = '\0';
            return true;
        }
        if (c == '\\')
        {
            j->p++;
            if (j->p >= j->end)
                break;
            char e = *j->p++;
            switch (e)
            {
            case '"':  PutUtf8('"',  &o); break;
            case '\\': PutUtf8('\\', &o); break;
            case '/':  PutUtf8('/',  &o); break;
            case 'b':  PutUtf8('\b', &o); break;
            case 'f':  PutUtf8('\f', &o); break;
            case 'n':  PutUtf8('\n', &o); break;
            case 'r':  PutUtf8('\r', &o); break;
            case 't':  PutUtf8('\t', &o); break;
            case 'u':
            {
                int u = Hex4(j);
                if (u < 0)
                    goto broken;
                // A character outside the basic plane arrives as a surrogate
                // PAIR, and emitting the halves separately produces two
                // three-byte sequences that are not the character and are not
                // valid UTF-8 either. Aliases are chosen by strangers and
                // emoji are common, so this path is real rather than
                // theoretical.
                if (u >= 0xD800 && u <= 0xDBFF)
                {
                    if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u')
                    {
                        const char *save = j->p;
                        j->p += 2;
                        int lo = Hex4(j);
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            unsigned int cp = 0x10000u +
                                              (((unsigned int)u - 0xD800u) << 10) +
                                              ((unsigned int)lo - 0xDC00u);
                            PutUtf8(cp, &o);
                            break;
                        }
                        j->p = save;    // not a pair after all
                    }
                    // A lone high surrogate. U+FFFD, which is what a decoder
                    // with errors="replace" produces for the same input.
                    PutUtf8(0xFFFD, &o);
                    break;
                }
                if (u >= 0xDC00 && u <= 0xDFFF)
                {
                    PutUtf8(0xFFFD, &o);        // a lone low surrogate
                    break;
                }
                PutUtf8((unsigned int)u, &o);
                break;
            }
            default:
                goto broken;
            }
            continue;
        }
        if (c < 0x20)
            break;              // a raw control character is not legal in a string

        // Raw bytes, which is how the map catalogue carries a name: json.dumps
        // with ensure_ascii=False writes the UTF-8 straight out. Copied a whole
        // sequence at a time, not a byte at a time, so that running out of room
        // cannot leave a lead byte with nothing after it.
        {
            int seq = 1;
            if (c >= 0xF0)      seq = 4;
            else if (c >= 0xE0) seq = 3;
            else if (c >= 0xC0) seq = 2;
            if (j->p + seq > j->end)
                break;          // the sequence runs off the end of the document
            Append(&o, j->p, seq);
            j->p += seq;
        }
    }

broken:
    Fail(j);
    if (out && cap > 0)
        out[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

static bool IsNumberStart(char c)
{
    return c == '-' || (c >= '0' && c <= '9');
}

WrJsonKind WrJsonPeek(WrJson *j)
{
    if (j->bad || !j->value)
        return WR_JSON_NONE;
    const char *save = j->p;
    j->p = j->value;
    char c = Peek(j);
    j->p = save;

    switch (c)
    {
    case '{': return WR_JSON_OBJECT;
    case '[': return WR_JSON_ARRAY;
    case '"': return WR_JSON_STRING;
    case 't': case 'f': return WR_JSON_BOOL;
    case 'n': return WR_JSON_NULL;
    default: break;
    }
    if (!IsNumberStart(c))
        return WR_JSON_NONE;

    // int or real, decided the way Python decides it: a '.' or an exponent
    // makes it a float, and read_map_catalogue's isinstance(mid, int) then
    // rejects it. Reproducing that here is the whole reason this is not a
    // library.
    const char *q = j->value;
    while (q < j->end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
        q++;
    for (; q < j->end; q++)
    {
        char n = *q;
        if (n == '.' || n == 'e' || n == 'E')
            return WR_JSON_REAL;
        if (!(n == '-' || n == '+' || (n >= '0' && n <= '9')))
            break;
    }
    return WR_JSON_INT;
}

// Step over whatever is under the cursor. Iterative: nesting is a counter, so
// a pathological document costs time and not stack.
static void SkipRaw(WrJson *j)
{
    char c = Peek(j);
    if (c == '\0')
    {
        Fail(j);
        return;
    }
    if (c == '"')
    {
        ReadString(j, NULL, 0);
        return;
    }
    if (c == '{' || c == '[')
    {
        int nest = 0;
        while (j->p < j->end)
        {
            char d = *j->p;
            if (d == '"')
            {
                if (!ReadString(j, NULL, 0))
                    return;
                continue;
            }
            j->p++;
            if (d == '{' || d == '[')
            {
                nest++;
                if (nest > WR_JSON_MAX_DEPTH)
                {
                    Fail(j);
                    return;
                }
            }
            else if (d == '}' || d == ']')
            {
                nest--;
                if (nest == 0)
                    return;
            }
        }
        Fail(j);
        return;
    }
    // A number, true, false or null: run to whatever ends it.
    while (j->p < j->end)
    {
        char d = *j->p;
        if (d == ',' || d == '}' || d == ']' ||
            d == ' ' || d == '\t' || d == '\r' || d == '\n')
            break;
        j->p++;
    }
}

void WrJsonSkip(WrJson *j)
{
    if (j->bad || !j->value)
        return;
    j->p = j->value;
    j->value = NULL;
    SkipRaw(j);
}

// Whatever the caller did not read. Called at the top of every Next*, which is
// what makes "forgot to consume it" not a bug you can write.
static void DropPending(WrJson *j)
{
    if (j->value)
        WrJsonSkip(j);
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

static bool Enter(WrJson *j, char open)
{
    if (j->bad || !j->value)
        return false;
    const char *save = j->p;
    j->p = j->value;
    if (Peek(j) != open)
    {
        j->p = save;
        return false;
    }
    if (j->depth >= WR_JSON_MAX_DEPTH)
    {
        Fail(j);
        return false;
    }
    j->p++;
    j->depth++;
    j->value = NULL;
    return true;
}

bool WrJsonEnterArray(WrJson *j)  { return Enter(j, '['); }
bool WrJsonEnterObject(WrJson *j) { return Enter(j, '{'); }

// Common half of NextElement and NextMember: settle on the next item, or
// consume the closing bracket and say there is none.
static bool NextSlot(WrJson *j, char close)
{
    if (j->bad)
        return false;
    DropPending(j);

    char c = Peek(j);
    if (c == close)
    {
        j->p++;
        j->depth--;
        return false;
    }
    if (c == ',')
    {
        j->p++;
        c = Peek(j);
        // A trailing comma. Python's json refuses it and so do we, rather than
        // quietly reading one fewer element than the file claims to have.
        if (c == close)
        {
            Fail(j);
            return false;
        }
    }
    if (c == '\0')
    {
        Fail(j);
        return false;
    }
    return true;
}

bool WrJsonNextElement(WrJson *j)
{
    if (!NextSlot(j, ']'))
        return false;
    j->value = j->p;
    return true;
}

bool WrJsonNextMember(WrJson *j, char *key, int keyCap)
{
    if (key && keyCap > 0)
        key[0] = '\0';
    if (!NextSlot(j, '}'))
        return false;

    if (!ReadString(j, key, keyCap))
    {
        Fail(j);
        return false;
    }
    if (Peek(j) != ':')
    {
        Fail(j);
        return false;
    }
    j->p++;
    SkipWs(j);
    j->value = j->p;
    return true;
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

long long WrJsonInt(WrJson *j, long long def, bool *ok)
{
    if (ok)
        *ok = false;
    if (WrJsonPeek(j) != WR_JSON_INT)
    {
        WrJsonSkip(j);
        return def;
    }
    j->p = j->value;
    SkipWs(j);
    char *stop = NULL;
    long long v = _strtoi64(j->p, &stop, 10);
    if (stop == j->p)
    {
        WrJsonSkip(j);
        return def;
    }
    j->p = stop;
    j->value = NULL;
    if (ok)
        *ok = true;
    return v;
}

double WrJsonReal(WrJson *j, double def, bool *ok)
{
    if (ok)
        *ok = false;
    WrJsonKind k = WrJsonPeek(j);
    if (k != WR_JSON_REAL && k != WR_JSON_INT)
    {
        WrJsonSkip(j);
        return def;
    }
    j->p = j->value;
    SkipWs(j);
    char *stop = NULL;
    double v = strtod(j->p, &stop);
    if (stop == j->p)
    {
        WrJsonSkip(j);
        return def;
    }
    j->p = stop;
    j->value = NULL;
    if (ok)
        *ok = true;
    return v;
}

bool WrJsonBool(WrJson *j, bool def, bool *ok)
{
    if (ok)
        *ok = false;
    if (WrJsonPeek(j) != WR_JSON_BOOL)
    {
        WrJsonSkip(j);
        return def;
    }
    j->p = j->value;
    bool v = (Peek(j) == 't');
    j->value = NULL;
    SkipRaw(j);
    if (ok)
        *ok = true;
    return v;
}

bool WrJsonString(WrJson *j, char *out, int cap)
{
    if (out && cap > 0)
        out[0] = '\0';
    if (WrJsonPeek(j) != WR_JSON_STRING)
    {
        WrJsonSkip(j);
        return false;
    }
    j->p = j->value;
    j->value = NULL;
    return ReadString(j, out, cap);
}
