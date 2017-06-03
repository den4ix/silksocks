#include "common.h"

unsigned char *hosts[] = {
    "google.com",
    "google.com",
    "amazon.com",
    "facebook.com",
    "yandex.ru",
    "yandex.ru",
    "github.com",
    "stackoverflow.com",
    "coursera.org",
    "wikipedia.org",
    "youtube.com",
    "bash.org",
    "yahoo.com",
    "news.ycombinator.com",
    "linuxjournal.com",
};

int main(int argc, char **argv)
{
    struct hostent *hent;
    uint32_t addr;
    time_t expires;

    hashtbl_init(&dns_table, 8, 4);
    hashtbl_print(&dns_table);  

    int i;
    
    expires = time(NULL)+1;
    printf("Time now: %lu\nExpires at: %lu\n", time(NULL), expires);
    for (i = 0; i < sizeof(hosts)/sizeof(*hosts); ++i) {
        if ((hent = gethostbyname(hosts[i])) != NULL) {
            addr = ((struct in_addr*)hent->h_addr)->s_addr;
            hashtbl_put(&dns_table, hosts[i], (unsigned char*)&addr, time(NULL)+1);
            printf("Time now: %lu\n", time(NULL));
            printf("Put: %s\n", hosts[i]);
            hashtbl_print(&dns_table);
        }
    }

    printf("=========\n");

    for (i = 0; i < sizeof(hosts)/sizeof(*hosts); ++i) {
        addr = ((struct in_addr*)hent->h_addr)->s_addr;
        printf("Time now: %lu\n", time(NULL));
        if (hashtbl_get(&dns_table, hosts[i], (unsigned char*)&addr)) {
            printf("Get: [%s] -> %s (success)\n", hosts[i], inet_ntoa(*(struct in_addr*)&addr));
        } else {
            printf("Get: [%s] -> null (fail)\n", hosts[i]);
        }
        hashtbl_print(&dns_table);
    }

}
