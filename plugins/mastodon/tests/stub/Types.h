/* Just enough of the Mac's Types.h for the Plug-In's Plugin.c on a host (e2e.c) */
#ifndef STUB_TYPES_H
#define STUB_TYPES_H
#define pascal
typedef short OSErr;
typedef unsigned int OSType;
typedef unsigned char Boolean;
typedef struct { short v, h; } Point;
enum { noErr = 0, paramErr = -50, memFullErr = -108, userCanceledErr = -128, fnfErr = -43,
       connectionClosing = -23005 };
#endif
