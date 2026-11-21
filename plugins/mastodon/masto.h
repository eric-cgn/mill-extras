/* masto.h: the Mastodon Plug-In's addresses, and its pages made from the client API's JSON
   as it streams in. Plain C with no Mac calls, so tests/test_mastodon.c runs it on a host;
   Plugin.c gives it Mill's Write and ToMacRoman, and the network.

   Addresses (mastodon://HOST/PATH; any page but a post's takes ?max_id=ID for the next
   page):
     /                  the instance's local timeline
     /public            its federated timeline (everything it knows of)
     /home              the home timeline of the account logged in (a token)
     /tags/NAME         a hashtag
     /@USER             an account's posts (USER@OTHER for one from elsewhere)
     /@USER/ID          a post, with the thread around it
     /login             log in with an access token, or log out
   and, as POSTs from the Plug-In's own pages only:
     /post              a new post (status, spoiler_text, visibility, in_reply_to_id)
     /@USER/ID/act      do=Favorite, Unfavorite, Boost, or Unboost
     /login             token=... to log in; forget=... to log out */
#ifndef MASTO_H
#define MASTO_H

#include "json.h"
#include "content.h"

#define M_ID 40             /* a status or account ID (Mastodon's are numbers, snarc's hex) */
#define M_ACCT 96           /* user@host */
#define M_NAME 96           /* a display name, UTF-8 */
#define M_SPOILER 300       /* a content warning */
#define M_URL 256
#define M_DESC 160          /* a picture's description */
#define M_MEDIA 4           /* pictures shown for a post */
#define M_CONTENT 6144      /* a post's text as HTML 3.2; more is cut */
#define M_LIMIT 40          /* posts a page, at most */

/* ---- Addresses ---------------------------------------------------------- */

enum {
    RT_LOCAL, RT_PUBLIC, RT_HOME, RT_TAG, RT_ACCOUNT, RT_STATUS, RT_LOGIN,
    RT_POST, RT_ACT, RT_BAD
};

typedef struct {
    short kind;
    char user[M_ACCT];      /* RT_ACCOUNT, RT_STATUS, RT_ACT: USER or USER@OTHER */
    char id[M_ID];          /* RT_STATUS, RT_ACT */
    char tag[100];          /* RT_TAG, as the address has it (%XX kept) */
    char max_id[M_ID];      /* ?max_id=, or "" */
} Route;

/* A path ("/@user/123?max_id=9", query and all) as a Route; RT_BAD if it's none of them */
void route_parse(const char *path, Route *rt);

/* The API path for a route's posts (RT_LOCAL, RT_PUBLIC, RT_HOME, RT_TAG; RT_ACCOUNT with
   the account's ID in id), limit at a time: its length, or -1 if it won't fit */
long route_api(const Route *rt, const char *id, int limit, char *out, long size);

/* ---- Pages --------------------------------------------------------------- */

/* Where a page goes. write returns 0 when no more is wanted; to_mac makes UTF-8 Mac
   Roman (MillPlugIn.h's ToMacRoman): the length, or -1 if it won't fit. */
typedef struct {
    void *ref;
    int (*write)(void *ref, const char *p, long n);
    long (*to_mac)(void *ref, const char *in, long len, char *out, long size);
} MOut;

typedef struct {
    char type[12];          /* image, gifv, video, audio, unknown */
    char url[M_URL];
    char preview[M_URL];
    char desc[M_DESC];
} MMedia;

typedef struct {
    char id[M_ID];
    char created[32];
    char acct[M_ACCT];
    char name[M_NAME];
    char spoiler[M_SPOILER];
    char reply_to[M_ID];
    char visibility[12];
    char url[M_URL];
    long replies, boosts, favs;
    char faved, boosted, any;
    short nmedia, moremedia;
} MStatus;

typedef struct {
    char id[M_ID];
    char acct[M_ACCT];
    char name[M_NAME];
    char url[M_URL];
    long followers, following, statuses;
    char any;
} MAccount;

/* What a reply is read as */
enum {
    MP_STATUSES,        /* an array of posts: a timeline */
    MP_STATUS,          /* one post */
    MP_ANCESTORS,       /* a context's ancestors (reading stops at its descendants) */
    MP_DESCENDANTS,     /* a context's descendants */
    MP_ACCOUNT          /* an account (lookup, verify_credentials) */
};

/* How posts are shown */
enum {
    MF_LOGGED = 1,      /* the actions are offered (a token for this instance) */
    MF_PICTURES = 2,    /* pictures as IMGs (else links) */
    MF_FOCUS = 4,       /* the post a thread is about: its content warning open, a reply form */
    MF_INDENT = 8       /* a reply in a thread */
};

typedef struct {
    const MOut *out;
    const char *host;
    short parse;            /* MP_... */
    short flags;            /* MF_... */
    JsonReader json;
    JsonSink sink;
    short depth;
    unsigned char role[JSON_DEPTH + 2];
    short media_at[2];      /* the media slot being read, per status */
    MStatus st[2];          /* [0] the post, [1] what it boosts */
    MMedia media[2][M_MEDIA];
    MAccount acct;          /* MP_ACCOUNT's */
    char error[200];        /* a reply's "error" (UTF-8) */
    Content content;
    short owner;            /* whose content is in cbuf: 0, 1 (statuses), 2 (the account's note) */
    short in_string;        /* a string being read into content */
    char cbuf[M_CONTENT];
    long count;             /* posts shown */
    char last_id[M_ID];     /* the last post shown's ID, for the next page */
    short stopped;          /* writing was refused */
} Model;

/* A reply's body, as parse; json_feed's answers (JSON_STOP when the reading is done:
   the ancestors read, or writing refused) */
void model_begin(Model *m, const MOut *out, const char *host, short parse, short flags);
int model_data(Model *m, const char *p, long n);
int model_end(Model *m);

/* Page parts, in Mac Roman. head: the HTML's start, title and nav (which page is on,
   whether logged in: who as, UTF-8 acct, or NULL). */
int page_head(const MOut *o, const char *host, const char *title, const Route *rt, const char *who);
/* A post form (in reply to reply_to, if not NULL) */
int page_post_form(const MOut *o, const char *host, const char *reply_to, const char *reply_acct);
/* The account read (m->acct and its note in m->cbuf) as a page's heading */
int page_account(const MOut *o, Model *m);
/* "Next page" to path?max_id=last, if a page's worth came */
int page_next(const MOut *o, const char *host, const char *path, Model *m, int limit);
int page_end(const MOut *o);
/* Plain text (UTF-8) as Mac Roman HTML */
int page_text(const MOut *o, const char *utf8);
int page_put(const MOut *o, const char *s);

#endif
