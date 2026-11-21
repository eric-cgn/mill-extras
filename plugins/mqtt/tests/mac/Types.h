/* Types.h for host tests: what MillPlugIn.h and the MQTT Plug-In use of the Mac's */
#ifndef TYPES_H
#define TYPES_H
#define pascal
typedef short OSErr;
typedef unsigned int OSType;
typedef unsigned char Boolean;
typedef struct Point { short v, h; } Point;
enum { noErr = 0, unimpErr = -4, ioErr = -36, fnfErr = -43, paramErr = -50, memFullErr = -108, userCanceledErr = -128 };
#endif
