/* mqtt.h: what the MQTT Plug-In's parts share. Its names are here and only here: MQTT_NAME
   for plugin://NAME/ (its About page and icons), and MQTT_TITLE, the title on its pages.
   The file's own name ("MQTT") is in build.sh and MQTT.r. */
#ifndef MQTT_H
#define MQTT_H

#define MQTT_NAME "mqtt"
#define MQTT_TITLE "MQTT"

/* mqtt.c: the protocol's description and settings, and a request */
const MillProtocol *mqtt_protocol(void);
OSErr mqtt_request(const MillHost *host, MillRequest *r);

#endif
