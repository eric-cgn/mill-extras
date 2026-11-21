/* show.c: a file through the converter, to look at: show md|gmi FILE */
#include "harness.h"
int main(int argc, char **argv)
{
    static char buf[1 << 20];
    FILE *f = fopen(argv[2], "rb");
    long n = fread(buf, 1, sizeof buf, f);
    fputs(convert(!strcmp(argv[1], "md") ? TX_MARKDOWN : TX_GEMTEXT, TX_UTF8, buf, n, argc > 3 ? atol(argv[3]) : 0), stdout);
    return 0;
}
