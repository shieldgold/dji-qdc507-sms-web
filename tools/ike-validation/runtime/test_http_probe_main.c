#include "http_probe.h"
int main(int argc, char **argv) {
    return argc == 2 ? qdc_http_once(argv[1]) : 64;
}
