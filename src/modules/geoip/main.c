#include <stdio.h>
#include <string.h>

#include "GeoIP.h"
#include "GeoIPCity.h"


enum { PRIV_NONE = 0, PRIV_MASTER, PRIV_AUTH, PRIV_ADMIN, PRIV_ROOT };

//void *(* z_getext)(char *);
//void (* z_setext)(char *, void *);

char dbfile[260]  = "GeoIP.dat";
int dbfileset = 0;
char cdbfile[260] = "GeoLiteCity.dat";
int cdbfileset = 0;

struct hookparam
{
    void *args[16];
} __attribute__((packed));


typedef void *(* getexttype)(char *);
typedef void (* setexttype)(char *, void *);
typedef void (* notifyprivtype)(char *, int, int);
typedef void (* debugtype)(char *);
typedef void (* addhooktype)(char *, int (*hookfunc)(struct hookparam *));
typedef void (* delhooktype)(char *, int (*hookfunc)(struct hookparam *));
typedef void (* logoutftype)(const char *fmt, ...);

/*
//void *(* sendf)(int, int, char *, ...);
void (*debug)(char *);
void (*notifypriv)(char *, int, int);
void (*addhook)(char *, int (*hookfunc)(struct hookparam *));
void (*delhook)(char *, int (*hookfunc)(struct hookparam *));
*/

getexttype z_getext = NULL;
setexttype z_setext = NULL;
notifyprivtype notifypriv = NULL;
debugtype debug = NULL;
addhooktype addhook = NULL;
delhooktype delhook = NULL;
logoutftype logoutf = NULL;

GeoIP *gi  = 0;
GeoIP *gic = 0;

int needcity = 1;
int needregion = 0;

int useunicode = 0;
int banproxies = 0;

int _argsep(char *str, int c, char **argv)
{
    char *s;
    int argc;
    int i;

    for(i = 1; i < c; i++) argv[i] = 0; //zero out all pointers
    argv[0] = str;
    if(!str || !str[0]) return 0;
    argc = 1;
    for(i = 1; i < c; i++)
    {
        s = strchr(argv[i - 1], ' ');
        if(!s) break;   //no delimiter found - prevous argument is last argument or string end
        *s = 0;         //replace delimiter with null
        s++;            //thing after delimiter
        while(*s == ' ') s++;   //skip other delimiters if any
        argv[i] = s;    //thing after all delimiters
        argc++;
    }
    return argc;
}


//0 - cn (unused in this hook)
//1 - name
//2 - ip
//3 - hostname
//4 - ban?

static char connmsg[260];
static char ipaddr[16];

int on_connect(struct hookparam *hp)
{
    const char *name = (const char *)hp->args[1];

    if(!name || !name[0] || !hp->args[2]) return 0;

    unsigned int ip = *(unsigned int *)hp->args[2];

    int searchcountry = gi ? 1 : 0;
    int searchcity = gic && needcity ? 1 : 0;
    int searchregion = gic && needregion ? 1 : 0;
    const char *country = 0, *city = 0, *region = 0;
    GeoIPRecord *gir = 0;
    const char *addrp;  //ptr to ip address string

    if(hp->args[3]) addrp = (const char *)hp->args[3];
    else
    {
        sprintf(ipaddr, "%u.%u.%u.%u",
                (ip&0xFF),
                ((ip>>8)&0xFF),
                ((ip>>16)&0xFF),
                ((ip>>24)&0xFF));
        addrp = ipaddr;
    }

    //check for reserved ip addresses
    if     ((ip & 0x000000FF) == 0x0000007F) country = "localhost";                     //127.*.*.*
    else if((ip & 0x0000FFFF) == 0x0000A8C0 || (ip & 0xFF) == 0x0A) country = "LAN";    //192.168.*.*, 10.*.*.*
    else if((ip & 0x00FFFFFF) == 0x006358C0) country = "6to4 relay";                    //192.88.99.*
    if(country) searchregion = searchcity = searchcountry = 0;  //do not do GeoIP lookup for reserved ip addresses

    //Get country name
    if(searchcountry)
    {
        country = GeoIP_country_name_by_addr(gi, addrp);
        if(country && strstr(country, "Proxy"))
        {
            searchcity = searchregion = 0;  //do not resolve cities and regions for proxies
            if(banproxies && !hp->args[4]) hp->args[4] = (void *)country;   //set ban reason
        }
    }

    //Get city and region name
    if(searchcity || searchregion)
    {
        gir = GeoIP_record_by_addr(gic, addrp);
        if(gir)
        {
            if(searchcity && gir->city && gir->city[0]) city = gir->city;
            if(searchregion) region = GeoIP_region_name_by_code(gir->country_code, gir->region);
        }
    }

    //assemble announcement string
    strcpy(connmsg, name);
    strcat(connmsg, " \f1connected from \f0");

    int first = 1;

    if(city)
    {
        first = 0;
        strcat(connmsg, city);
    }

    if(region && (!city || strcmp(city, region)))
    {
        if(first) first = 0;
        else strcat(connmsg, "\f1, \f0");
        strcat(connmsg, region);
    }

    //city and region strings copied, lets free gir structure
    if(gir) GeoIPRecord_delete(gir);

    if(country)
    {
        if(first) first = 0;
        else strcat(connmsg, "\f1, \f0");
        strcat(connmsg, country);
    }

    if(!first) notifypriv(connmsg, PRIV_NONE, PRIV_AUTH);

    //add ip address to annoncement string and notify admins
    if(!first) strcat(connmsg, " \f1[\f0");
    strcat(connmsg, addrp);
    if(!first) strcat(connmsg, "\f1]");

    if(logoutf) logoutf(connmsg);

    notifypriv(connmsg, PRIV_ADMIN, PRIV_ROOT);

    return 0;
}

char *z_init(void *getext, void *setext, char *args)
{
    int argc;
    char *argv[16];
    int i;
    int nomem = 0;

    z_getext = (getexttype)getext;
    z_setext = (setexttype)setext;

    argc = _argsep(args, 16, argv);
    for(i = 0; i < argc; i++) switch(argv[i][0])
    {
        case 'f':
            strncpy(dbfile, argv[++i], 260);
            dbfileset = 1;
            break;

        case 'c':
            strncpy(cdbfile, argv[++i], 260);
            cdbfileset = 1;
            break;

        case 'n':
            nomem = 1;
            break;

        case 'r':
            needregion = 1;
            break;

        case 'p':
            needcity = 0;
            break;

        case 'u':
            useunicode = 1;
            break;

        case 'b':
            banproxies = 1;
            break;

        default:
            return "Unknown switch";
    }

    notifypriv = (notifyprivtype)z_getext("notifypriv");
    if(!notifypriv) return "Can't find notifypriv server API entry";

    debug = (debugtype)z_getext("debug");

    logoutf = (logoutftype)z_getext("logoutf");

    addhook = (addhooktype)z_getext("addhook");
    if(!addhook) return "Can't find addhook server API entry";

    delhook = (delhooktype)z_getext("delhook");
    if(!delhook && debug) debug("Can't find delhook server API entry");

    if(!gi) gi = GeoIP_open(dbfile, !nomem ? GEOIP_MEMORY_CACHE : GEOIP_STANDARD);
    if(!gi)
    {
        if(dbfileset) return "Failed to load GeoIP database";
        else if(debug) debug("Failed to load GeoIP database (GeoIP.dat)");
    }

    if(!gic && (needcity || needregion)) gic = GeoIP_open(cdbfile, !nomem ? GEOIP_INDEX_CACHE : GEOIP_STANDARD);
    if(!gic && (needcity || needregion))
    {
        if(cdbfileset)
        {
            if(gi) GeoIP_delete(gi);
            return "Failed to load GeoIPCity database";
        }
        else if(debug) debug("Failed to load GeoIPCity database (GeoLiteCity.dat)");
    }

    if(useunicode)
    {
        if(gi) GeoIP_set_charset(gi, GEOIP_CHARSET_UTF8);
        if(gic) GeoIP_set_charset(gic, GEOIP_CHARSET_UTF8);
    }

    addhook("connected", on_connect);

    return 0;
}

char *z_uninit()
{
    if(delhook) delhook("connected", on_connect);

    if(gi)
    {
        GeoIP_delete(gi);
        gi = 0;
    }

    if(gic)
    {
        GeoIP_delete(gic);
        gic = 0;
    }

    //causes incompatibility with older operating systems
#if 0
    GeoIP_cleanup();
#endif

    return 0;
}
