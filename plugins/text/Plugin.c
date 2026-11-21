/* Modern Text: a Plug-In for Mill that shows Markdown and Gemtext as pages. The
   converters are text.h's; this gives them Mill's memory, Write, and ToMacRoman. Its
   About page is its 'TEXT' 128. */
#include <string.h>
#include <Memory.h>

#include "MillPlugIn.h"
#include "text.h"

#define FORMATS 2

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillFormat formats[FORMATS];
} Text;

/* One page */
typedef struct {
    const MillHost *host;
    MillFormatRef ref;
} Run;

static void *run_alloc(void *ref, long n) { return ((Run *)ref)->host->NewMem(n); }
static void run_release(void *ref, void *p) { ((Run *)ref)->host->FreeMem(p); }

static int run_write(void *ref, const char *p, long n)
{
    Run *r = ref;
    return r->host->Write((MillReqRef)r->ref, p, n) != 0;
}

static long run_to_mac(void *ref, const char *in, long len, short cs, char *out, long size)
{
    return ((Run *)ref)->host->ToMacRoman(in, len, cs, out, size);
}

/* f->state: the Run, with the converter's run after it */
typedef struct {
    Run run;
    TxSink sink;
    void *tx;
} State;

static OSErr text_format(MillPlugIn *self, short selector, MillFormatParams *f)
{
    State *p = f->state;
    switch (selector) {
    case millFormatBegin:
        if (!(p = self->host->NewMem(sizeof *p))) return memFullErr;
        p->run.host = self->host;
        p->run.ref = f->ref;
        p->sink.ref = &p->run;
        p->sink.alloc = run_alloc;
        p->sink.release = run_release;
        p->sink.write = run_write;
        p->sink.to_mac = run_to_mac;
        if (!(p->tx = tx_begin(f->format == 0 ? TX_MARKDOWN : TX_GEMTEXT, tx_charset(f->type), &p->sink))) {
            self->host->FreeMem(p);
            return memFullErr;
        }
        f->state = p;
        f->answer = millShowNow;
        return noErr;
    case millFormatData:
        return tx_data(p->tx, f->data, f->len) == TX_MORE ? noErr : millDone;
    case millFormatEnd:
        if (!p) return noErr;
        tx_end(p->tx);      /* what's open is closed, cut short or not */
        self->host->FreeMem(p);
        f->state = NULL;
        return noErr;
    }
    return millUnknown;
}

static void text_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void format(MillFormat *f, const char *key, const char *heading, const char *description,
                   const char *types, const char *extensions)
{
    f->key = key;
    f->heading = heading;
    f->description = description;
    f->types = types;
    f->extensions = extensions;
    f->fileTypes = "";
    f->gives = millGivesElements;
    f->settingCount = 0;
    f->settings = 0;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Text *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = text_close;
    me->methods.request = 0;
    me->methods.purge = 0;
    me->methods.format = text_format;
    me->methods.font = 0;
    me->base.methods = &me->methods;
    format(&me->formats[0], "markdown", "Markdown", "CommonMark, with tables (text/markdown)",
           "text/markdown text/x-markdown", "md markdown");
    format(&me->formats[1], "gemtext", "Gemtext", "Gemini (text/gemini)", "text/gemini", "gmi gemini");
    info->name = "modern-text";
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Markdown and Gemtext as pages.";
    info->flags = 0;
    info->protocolCount = 0;
    info->protocols = 0;
    info->formatCount = FORMATS;
    info->formats = me->formats;
    return &me->base;
}
