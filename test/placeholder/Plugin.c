/* NOT FOR RELEASE. A placeholder Plug-In that only tests the Mill Extras disk and installer
   (EXTRAS_PLACEHOLDER=1 ./build.sh). placeholder:// answers with one line of text. */
#include "MillPlugIn.h"

typedef struct {
    MillPlugIn base;
    MillMethods methods;
    MillProtocol protocol;
} Placeholder;

static void ph_close(MillPlugIn *self)
{
    self->host->FreeMem(self);
}

static OSErr ph_request(MillPlugIn *self, MillRequest *r)
{
    static const char page[] = "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/plain" MILL_CRLF MILL_CRLF
        "Placeholder: a test of the Mill Extras installer. Not for release." MILL_CRLF;
    if (r->protocol < 0) return millUnknown;
    self->host->Write(r->ref, page, sizeof page - 1);
    return noErr;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Placeholder *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = ph_close;
    me->methods.request = ph_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->base.methods = &me->methods;
    me->protocol.scheme = "placeholder";
    me->protocol.heading = "Placeholder";
    me->protocol.description = "Test only, not for release";
    me->protocol.blockSize = 0;
    me->protocol.settingCount = 0;
    me->protocol.settings = 0;
    info->name = "placeholder";
    info->version = "0.0";
    info->copyright = "Test";
    info->by = "OneInAMillion Software";
    info->description = "TEST ONLY: tests the Mill Extras installer. Not for release.";
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    return &me->base;
}
