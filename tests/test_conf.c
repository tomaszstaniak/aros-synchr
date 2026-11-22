/* The configuration file (conf.c): what is accepted, and that anything
 * not understood refuses the whole file with its line number. */
#include "../src/conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define ID_A "MFZWI3D-BONSGYC-YLTMRWG-C43ENR5-QXGZDMM-FZWI3DP-BONSGYY-LTMRWAD"
#define ID_B "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ2"

static void refused(const char *text, const char *expect)
{
    conf c;
    char why[300] = "";
    int rc = conf_parse(text, &c, why, sizeof why);
    CHECK(rc != 0, "accepted: %s", text);
    CHECK(strstr(why, expect) != NULL, "reason \"%s\" lacks \"%s\"", why, expect);
    CHECK(c.ndevices == 0 && c.nfolders == 0 && !c.name, "something left after a refusal");
}

int main(void)
{
    conf c;
    char why[300];

    const char *good =
        "# two devices, three folders\n"
        "name pegasos\n"
        "\n"
        "device office\n"
        "  id " ID_A "\n"
        "  address 192.168.1.10:22000\n"
        "folder documents\n"
        "\tpath Work:My Documents \n"
        "  with office\n"
        "device laptop\r\n"
        "  address 10.0.2.2:22100\r\n"
        "  id " ID_B "\r\n"
        "folder music\n"
        "  with laptop\n"
        "  path Work:Music/\n"
        "  rescan 300\n"
        "folder notes\n"
        "  path RAM:notes\n"
        "  with office";
    CHECK(conf_parse(good, &c, why, sizeof why) == 0, "good file refused: %s", why);
    CHECK(c.ndevices == 2 && c.nfolders == 3, "counts %lu %lu", (unsigned long)c.ndevices, (unsigned long)c.nfolders);
    if (c.ndevices == 2 && c.nfolders == 3) {
        CHECK(strcmp(c.name, "pegasos") == 0, "name %s", c.name);
        CHECK(strcmp(c.devices[0].name, "office") == 0 && strcmp(c.devices[0].ip, "192.168.1.10") == 0
              && c.devices[0].port == 22000, "device 0");
        CHECK(strcmp(c.devices[1].name, "laptop") == 0 && strcmp(c.devices[1].ip, "10.0.2.2") == 0
              && c.devices[1].port == 22100, "device 1 (CR LF lines)");
        CHECK(memcmp(c.devices[0].id, c.devices[1].id, DEVID_BYTES) != 0, "ids equal");
        CHECK(strcmp(c.folders[0].path, "Work:My Documents") == 0, "path with a space: \"%s\"", c.folders[0].path);
        CHECK(c.folders[0].device == 0 && c.folders[1].device == 1 && c.folders[2].device == 0, "devices of folders");
        CHECK(c.folders[0].rescan_seconds == CONF_RESCAN_DEFAULT && c.folders[1].rescan_seconds == 300, "rescan");
        CHECK(strcmp(c.folders[2].id, "notes") == 0 && strcmp(c.folders[2].path, "RAM:notes") == 0, "last line");
    }
    conf_free(&c);

    const char *min = "device d\nid " ID_A "\naddress 1.2.3.4:5\nfolder f\npath /x\nwith d\n";
    CHECK(conf_parse(min, &c, why, sizeof why) == 0, "minimal file refused: %s", why);
    CHECK(c.name && strcmp(c.name, "aros") == 0, "default name");
    conf_free(&c);

    /* A dynamic address: the word, or no address line (which used to be
     * refused as "no address"); both are ip NULL and port 0. */
    {
        const char *word = "device d\nid " ID_A "\naddress dynamic\nfolder f\npath /x\nwith d\n";
        const char *none = "device d\nid " ID_A "\nfolder f\npath /x\nwith d\n"
                           "device e\naddress 1.2.3.4:5\nid " ID_B "\n";
        CHECK(conf_parse(word, &c, why, sizeof why) == 0, "address dynamic refused: %s", why);
        CHECK(c.ndevices == 1 && c.devices[0].ip == NULL && c.devices[0].port == 0, "address dynamic: ip and port");
        conf_free(&c);
        CHECK(conf_parse(none, &c, why, sizeof why) == 0, "a device without an address line refused: %s", why);
        CHECK(c.ndevices == 2 && c.devices[0].ip == NULL && c.devices[0].port == 0, "no address line: ip and port");
        CHECK(c.ndevices == 2 && c.devices[1].ip && strcmp(c.devices[1].ip, "1.2.3.4") == 0 && c.devices[1].port == 5,
              "a typed address next to a dynamic one");
        conf_free(&c);
    }

#define DEV "device d\nid " ID_A "\naddress 1.2.3.4:5\n"
    refused("", "no folder");
    refused(DEV, "no folder");
    refused(DEV "folder f\npath /x\n", "names no device");
    refused(DEV "folder f\nwith d\n", "no path");
    refused(DEV "folder f\npath /x\nwith e\n", "not defined");
    refused(DEV "folder f\npath /x\nwith d\nwith d\n", "one device");
    refused(DEV "folder f\npath /x\nwith d\nfolder f\npath /y\nwith d\n", "defined twice");
    refused(DEV "folder f\npath Work:Docs\nwith d\nfolder g\npath work:docs/\nwith d\n", "same path");
    refused(DEV DEV "folder f\npath /x\nwith d\n", "defined twice");
    refused(DEV "device e\nid " ID_A "\naddress 1.2.3.4:6\nfolder f\npath /x\nwith d\n", "same Device ID");
    refused("device d\naddress 1.2.3.4:5\nfolder f\npath /x\nwith d\n", "no id");
    refused("device d\nid " ID_A "\naddress dynamic\naddress 1.2.3.4:5\n", "line 4: address is given twice");
    refused("device d\nid " ID_A "\naddress 1.2.3.4:5\naddress dynamic\n", "line 4: address is given twice");
    refused("device d\nid " ID_A "\naddress Dynamic\n", "line 3");
    refused("device d\nid " ID_A "\naddress dynamic 1.2.3.4:5\n", "line 3");
    refused("device d\nid " ID_A "\naddress 1.2.3.4:0\n", "line 3");
    refused("device d\nid NOT-AN-ID\naddress 1.2.3.4:5\n", "line 2");
    refused("device d\nid " ID_A "\naddress example.org:22000\n", "line 3");
    refused("device d\nid " ID_A "\naddress 1.2.3.4:70000\n", "line 3");
    refused("device d\nid " ID_A "\naddress 1.2.3.4:5 x\n", "line 3");
    refused(DEV "folder f\npath /x\nwith d\nrescan 0\n", "line 7");
    refused(DEV "folder f\npath /x\nwith d\nrescan 60s\n", "line 7");
    refused(DEV "folder f\npath /x\nwith d\ncolour blue\n", "line 7: \"colour\"");
    refused(DEV "folder my folder\npath /x\nwith d\n", "no spaces");
    refused("path /x\n", "line 1");
    refused(DEV "folder f\npath\nwith d\n", "no value");
    refused("name a\nname b\n", "twice");

    CHECK(conf_load("/nonexistent/stpoc.conf", &c, why, sizeof why) != 0 && strstr(why, "cannot open"), "missing file");

    /* conf_save: what it writes is read back the same. */
    {
        const char *path = "build/host/test_conf.saved";
        conf a, b;
        CHECK(conf_parse("name me\n" DEV "device e\nid " ID_B "\naddress 10.0.0.2:22000\n"
                         "folder f\npath Work:My Documents\nwith e\nrescan 30\nfolder g\npath /x\nwith d\n",
                         &a, why, sizeof why) == 0, "save: source parses (%s)", why);
        CHECK(conf_save(path, &a, why, sizeof why) == 0, "save: written (%s)", why);
        CHECK(conf_save(path, &a, why, sizeof why) == 0, "save: written over the previous file (%s)", why);
        CHECK(conf_load(path, &b, why, sizeof why) == 0, "save: read back (%s)", why);
        CHECK(b.ndevices == 2 && b.nfolders == 2 && strcmp(b.name, "me") == 0, "save: counts and name");
        CHECK(b.nfolders == 2 && strcmp(b.folders[0].path, "Work:My Documents") == 0 && b.folders[0].device == 1
              && b.folders[0].rescan_seconds == 30 && b.folders[1].device == 0, "save: folders");
        CHECK(b.ndevices == 2 && memcmp(a.devices[1].id, b.devices[1].id, DEVID_BYTES) == 0 && b.devices[1].port == 22000
              && strcmp(b.devices[1].ip, "10.0.0.2") == 0, "save: devices");
        free(a.folders[0].id);
        a.folders[0].id = strdup("two words");
        CHECK(conf_save(path, &a, why, sizeof why) != 0, "save: a folder ID with a space is refused");
        conf_free(&b);
        CHECK(conf_load(path, &b, why, sizeof why) == 0 && strcmp(b.folders[0].id, "f") == 0, "save: a refused save leaves the file");
        conf_free(&a);
        conf_free(&b);
        conf_allow_no_folder = 1;
        CHECK(conf_parse(DEV, &a, why, sizeof why) == 0 && a.ndevices == 1 && a.nfolders == 0, "no folder allowed for an editor");
        CHECK(conf_save(path, &a, why, sizeof why) == 0 && conf_load(path, &b, why, sizeof why) == 0 && b.ndevices == 1,
              "save: devices only");
        conf_free(&a);
        conf_free(&b);
        conf_allow_no_folder = 0;

        /* A dynamic address is written as the word and read back as such;
         * half an address is refused. */
        CHECK(conf_parse("device d\nid " ID_A "\ndevice e\nid " ID_B "\naddress 10.0.0.2:22000\n"
                         "folder f\npath /x\nwith d\n", &a, why, sizeof why) == 0, "save dynamic: source parses (%s)", why);
        CHECK(conf_save(path, &a, why, sizeof why) == 0 && conf_load(path, &b, why, sizeof why) == 0,
              "save dynamic: written and read back (%s)", why);
        CHECK(b.ndevices == 2 && b.devices[0].ip == NULL && b.devices[0].port == 0 && b.devices[1].ip
              && strcmp(b.devices[1].ip, "10.0.0.2") == 0 && b.devices[1].port == 22000, "save dynamic: devices");
        {
            FILE *f = fopen(path, "r");
            char text[2000];
            size_t got = f ? fread(text, 1, sizeof text - 1, f) : 0;
            if (f)
                fclose(f);
            text[got] = 0;
            CHECK(strstr(text, "\n  address dynamic\n") != NULL, "save dynamic: the line \"address dynamic\" is written");
        }
        conf_free(&b);
        a.devices[0].port = 22000; /* a port without an address */
        CHECK(conf_save(path, &a, why, sizeof why) != 0, "save: a port without an address is refused");
        a.devices[0].port = 0;
        a.devices[1].port = 0; /* an address without a port */
        CHECK(conf_save(path, &a, why, sizeof why) != 0, "save: an address without a port is refused");
        conf_free(&a);
        remove(path);
    }

    /* How this device is reached: Syncthing's defaults without lines,
     * every option read, written and read back the same. */
    CHECK(conf_parse(min, &c, why, sizeof why) == 0, "minimal file refused: %s", why);
    CHECK(c.listen_port == CONF_LISTEN_DEFAULT && c.global_discovery && c.relays && c.local_announce
              && !c.discovery_server && !c.relay, "reachability defaults");
    conf_free(&c);
    const char *reach =
        "listen 22001\nglobal-discovery off\nrelays on\nlocal-announce off\n"
        "discovery-server https://127.0.0.1:8443/v2/\nrelay relay://127.0.0.1:22067/?id=" ID_B "\n"
        "device d\nid " ID_A "\nfolder f\npath /x\nwith d\n";
    CHECK(conf_parse(reach, &c, why, sizeof why) == 0, "reachability options refused: %s", why);
    CHECK(c.listen_port == 22001 && !c.global_discovery && c.relays && !c.local_announce, "reachability values");
    CHECK(c.discovery_server && strcmp(c.discovery_server, "https://127.0.0.1:8443/v2/") == 0, "discovery-server");
    CHECK(c.relay && strncmp(c.relay, "relay://127.0.0.1:22067/", 24) == 0, "relay");
    {
        const char *path = "test_conf_reach.tmp";
        conf back;
        CHECK(conf_save(path, &c, why, sizeof why) == 0, "save: %s", why);
        CHECK(conf_load(path, &back, why, sizeof why) == 0, "load back: %s", why);
        CHECK(back.listen_port == 22001 && !back.global_discovery && back.relays && !back.local_announce
                  && back.relay && strcmp(back.relay, c.relay) == 0 && back.discovery_server
                  && strcmp(back.discovery_server, c.discovery_server) == 0, "reachability round trip");
        conf_free(&back);
        remove(path);
    }
    conf_free(&c);
    CHECK(conf_parse("listen off\ndevice d\nid " ID_A "\nfolder f\npath /x\nwith d\n", &c, why, sizeof why) == 0
              && c.listen_port == 0, "listen off");
    conf_free(&c);
    refused("listen 70000\ndevice d\nid " ID_A "\nfolder f\npath /x\nwith d\n", "line 1: listen is a port");
    refused("relays maybe\ndevice d\nid " ID_A "\nfolder f\npath /x\nwith d\n", "line 1: relays is on or off");
    refused("relay tcp://1.2.3.4:5\ndevice d\nid " ID_A "\nfolder f\npath /x\nwith d\n", "line 1: relay is one");
    refused("discovery-server http://x/\ndevice d\nid " ID_A "\nfolder f\npath /x\nwith d\n",
            "line 1: discovery-server");
    refused("device d\nid " ID_A "\nlisten 22000\nfolder f\npath /x\nwith d\n", "line 3: \"listen\" is not known here");

    printf("test_conf: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
