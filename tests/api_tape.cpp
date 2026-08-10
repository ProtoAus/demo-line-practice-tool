// api_tape.cpp  --  see api_tape.h. NOT SHIPPED.

#include "api_tape.h"

#include "wr_api.h"
#include "wr_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Entry
{
    char url[4096];
    char file[64];
};

static char g_dir[MAX_PATH] = "";
static bool g_record = false;
static Entry *g_index = NULL;
static int g_indexCount = 0, g_indexCap = 0;
static int g_requests = 0;

static void IndexPath(char *out, int cap)
{
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s\\index.txt", g_dir);
}

static void MakeTree(const char *path)
{
    char buf[MAX_PATH];
    strcpy_s(buf, sizeof(buf), path);
    for (char *p = buf; *p; p++)
    {
        if (*p != '\\' && *p != '/')
            continue;
        char was = *p;
        *p = '\0';
        if (buf[0] && !(buf[1] == ':' && buf[2] == '\0'))
            CreateDirectoryA(buf, NULL);
        *p = was;
    }
    CreateDirectoryA(buf, NULL);
}

static void IndexAdd(const char *url, const char *file)
{
    if (g_indexCount >= g_indexCap)
    {
        int cap = g_indexCap ? g_indexCap * 2 : 64;
        Entry *grown = (Entry *)realloc(g_index, sizeof(Entry) * (size_t)cap);
        if (!grown)
            return;
        g_index = grown;
        g_indexCap = cap;
    }
    strncpy_s(g_index[g_indexCount].url, sizeof(g_index[0].url), url, _TRUNCATE);
    strncpy_s(g_index[g_indexCount].file, sizeof(g_index[0].file), file, _TRUNCATE);
    g_indexCount++;
}

// The reference's _tape_load: skip comments and anything with no tab, split
// once, keep the order. Written with newline="\n" there, so this is read with
// the carriage return stripped rather than assumed absent.
static void IndexLoad(void)
{
    char path[MAX_PATH];
    IndexPath(path, sizeof(path));

    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return;

    char line[4200];
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (line[0] == '#')
            continue;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        IndexAdd(line, tab + 1);
    }
    fclose(f);
}

bool WrTapeOpen(const char *dir, bool record)
{
    strncpy_s(g_dir, sizeof(g_dir), dir, _TRUNCATE);
    g_record = record;
    g_requests = 0;

    char path[MAX_PATH];
    IndexPath(path, sizeof(path));

    if (record)
    {
        MakeTree(g_dir);
        FILE *f = NULL;
        if (fopen_s(&f, path, "wb") != 0 || !f)
        {
            printf("[!] could not write %s\n", path);
            return false;
        }
        fputs("# WrLines: recorded API responses, in request order.\n", f);
        fclose(f);
        return true;
    }

    IndexLoad();
    if (g_indexCount == 0)
    {
        printf("[!] no recording in %s\n", dir);
        return false;
    }
    return true;
}

static bool ReadWhole(const char *path, unsigned char **out, size_t *lenOut,
                      char *err, int errCap)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
    {
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "could not read %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "could not size %s", path);
        return false;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "out of memory");
        return false;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    *lenOut = got;
    return true;
}

static bool Transport(void *, const char *url, unsigned char **out,
                      size_t *lenOut, char *err, int errCap)
{
    g_requests++;

    if (!g_record)
    {
        // First match wins, which is what a linear scan of the reference's list
        // of (url, name) pairs does. A board fetch never asks the same URL
        // twice in one run, so "first" and "only" are the same thing.
        for (int i = 0; i < g_indexCount; i++)
        {
            if (strcmp(g_index[i].url, url) != 0)
                continue;
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_dir,
                        g_index[i].file);
            return ReadWhole(path, out, lenOut, err, errCap);
        }
        // The reference raises MtvError("not in the recording: %s"), and
        // cmd_board's bare except prints str(e). This is that string, so the
        // line both sides print is the same line.
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "not in the recording: %s", url);
        return false;
    }

    if (!WrHttpGet(url, WrApiUserAgent(), out, lenOut, NULL, err, errCap))
        return false;

    char name[32];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "%04d.bin", g_requests);

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_dir, name);
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") == 0 && f)
    {
        fwrite(*out, 1, *lenOut, f);
        fclose(f);
    }

    IndexPath(path, sizeof(path));
    if (fopen_s(&f, path, "ab") == 0 && f)
    {
        fprintf(f, "%s\t%s\n", url, name);
        fclose(f);
    }
    return true;
}

void WrTapeInstall(void)
{
    WrApiSetTransport(Transport, NULL, g_record);
}

int WrTapeRequests(void) { return g_requests; }

void WrTapeClose(void)
{
    WrApiSetTransport(NULL, NULL, true);
    free(g_index);
    g_index = NULL;
    g_indexCount = g_indexCap = 0;
}
