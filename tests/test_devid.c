/* Device ID vectors from Syncthing's lib/protocol/deviceid_test.go. */
#include "../src/devid.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            failures++;                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

static const char formatted[] =
    "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ2";

static const char *valid[] = {
    "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ2",
    "P56IOI7 MZJNU2I QGDREYD M2MGTMGL 3BXNPQ6W 5BTB BZ4T JXZWICQ",
    "P56IOI7 MZJNU2Y IQGDREY DM2MGTI MGL3BXN PQ6W5BM TBBZ4TJ XZWICQ2",
    "P56IOI7MZJNU2IQGDREYDM2MGTMGL3BXNPQ6W5BTBBZ4TJXZWICQ",
    "p56ioi7mzjnu2iqgdreydm2mgtmgl3bxnpq6w5btbbz4tjxzwicq",
    "P56IOI7MZJNU2YIQGDREYDM2MGTIMGL3BXNPQ6W5BMTBBZ4TJXZWICQ2",
    "P561017MZJNU2YIQGDREYDM2MGTIMGL3BXNPQ6W5BMTBBZ4TJXZWICQ2", /* 0/1 look-alikes */
};

static const char *invalid[] = {
    "P56IOI7MZJNU2IQGDREYDM2MGTMGL3BXNPQ6W5BTBBZ4TJXZWICQCCCC", /* bad check chars */
    "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ3", /* last check char */
    "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ", /* 55 chars */
    "",
    "P56IOI7MZJNU2IQGDREYDM2MGTMGL3BXNPQ6W5BTBBZ4TJXZWICR", /* nonzero pad bits */
    "P56IOI7MZJNU2IQGDREYDM2MGTMGL3BXNPQ6W5BTBBZ4TJXZWIC!",
};

int main(void)
{
    unsigned char id[DEVID_BYTES], first[DEVID_BYTES];
    char text[DEVID_TEXT_SIZE];

    CHECK(devid_parse(formatted, first) == 0, "canonical form rejected");
    devid_format(first, text);
    CHECK(strcmp(text, formatted) == 0, "round trip gave %s", text);

    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        int rc = devid_parse(valid[i], id);
        CHECK(rc == 0, "valid[%zu] rejected: %s", i, valid[i]);
        if (rc == 0) {
            devid_format(id, text);
            CHECK(memcmp(id, first, DEVID_BYTES) == 0 && strcmp(text, formatted) == 0,
                  "valid[%zu] decoded to %s", i, text);
        }
    }
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        CHECK(devid_parse(invalid[i], id) != 0, "invalid[%zu] accepted: %s", i, invalid[i]);

    printf("%s: test_devid\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
