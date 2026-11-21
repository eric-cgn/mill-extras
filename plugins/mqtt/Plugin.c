/* MQTT: a Plug-In for Mill for MQTT brokers.
     mqtt://broker/topic        a topic's retained value, and a form to publish to it
     mqtt://broker/a/b/         every topic under a/b (a/b/#), as a table
   The protocol is mqtt.c, on mqttutil.c's packets. Its About page is its 'TEXT' 128. */
#include <string.h>

#include "MillPlugIn.h"
#include "mqtt.h"

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol;
} Mqtt1;

static void mqtt_close(MillPlugIn *self) { self->host->FreeMem(self); }

static OSErr plugin_request(MillPlugIn *self, MillRequest *r)
{
    if (r->protocol != 0) return millUnknown;
    return mqtt_request(self->host, r);
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Mqtt1 *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = mqtt_close;
    me->methods.request = plugin_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    me->protocol = *mqtt_protocol();
    info->name = MQTT_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "MQTT topics as pages, and publishing.";
    info->flags = 0;
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
