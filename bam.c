#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/sysctl.h>

#include <cmph.h>
#include <microhttpd.h>

#define errstr strerror(errno)

void die(const char * fmt, ...) {
  va_list args;
  va_start(args,fmt);
  vfprintf(stderr,fmt,args);
  va_end(args);
  exit(1);
}

static const char *const magic = "bam index: v000";

static char *index_file = NULL;
static int port = 8080;
static int threads = 0;
static int serve_data = 1;

static const char *usage = "bam [options] [tsv file]";
static const char *opts =
    " -i --index=<file>   Use <file> as index or \"-\" for none\n"
    " -p --port=<port>    Listen on TCP port number <port>\n"
    " -t --threads=<n>    Serve requests using <n> threads\n"
    " -x --exit           Index and exit without serving data\n"
    " -h --help           Print help message\n";

void parse_opts(int *argcp, char ***argvp) {
    static char* shortopts = "i:p:t:xh";
    static struct option longopts[] = {
        { "index",       required_argument, 0, 'i' },
        { "port",        required_argument, 0, 'p' },
        { "threads",     required_argument, 0, 't' },
        { "exit",        no_argument,       0, 'x' },
        { "help",        no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    while ((c = getopt_long(*argcp,*argvp,shortopts,longopts,0)) != -1) {
        switch (c) {
        case 'i':
            index_file = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            if (port <= 0) die("Invalid port number: %s\n", optarg);
            break;
        case 't':
            threads = atoi(optarg);
            if (threads <= 0) die("Invalid thread count: %s\n", optarg);
            break;
        case 'x':
            serve_data = 0;
            break;
        case 'h':
            printf("usage: %s\n%s", usage, opts);
            exit(0);
        case '?':
            fprintf(stderr, "options:\n%s", opts);
            exit(1);
        default:
            fprintf(stderr, "bam: unhandled option -- %c\n",  c);
            exit(1);
        }
    }
    *argvp += optind;
    *argcp -= optind;
}

void *mmap_file(const char *const file, struct stat *fs) {
    FILE *f = fopen(file,"r");
    if (!f) die("Error opening \"%s\": %s\n", file, errstr);
    if (fstat(fileno(f),fs)) die("Error fstating \"%s\": %s\n", file, errstr);
    if (!fs->st_size) die("Data file \"%s\" is empty\n", file);
    void *p = mmap(0, fs->st_size, PROT_READ, MAP_SHARED, fileno(f), 0);
    if (p == MAP_FAILED) die("Error mmapping \"%s\": %s\n", file, errstr);
    return p;
}

static char *data;
static off_t n = 0;
static off_t *offsets;
static cmph_t *hash;

static off_t keylen(const char *const key) { return strchr(key,'\t')-key; }
static off_t vallen(const char *const val) { return strchr(val,'\n')-val+1; }

#define idx(v)  (*((off_t*)v))
#define keys(i) (data + offsets[i])

static int key_read(void *v, char **key, cmph_uint32 *len) {
    return *len = keylen(*key = keys(idx(v)++));
}
static void key_rewind(void *v) { idx(v) = 0; }
static void key_dispose(void *v, char *key, cmph_uint32 len) { }

static char *const err = "Resource not found\n";

static int handle_request(
    void *cls,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **con_cls
) {
    int ret;
    struct MHD_Response *response;
    if (strcmp(method,"GET")) return MHD_NO;
    if (*url++ != '/') goto invalid_key;

    off_t url_len = strlen(url);
    cmph_uint32 h = cmph_search(hash, url, url_len);
    if (h >= n) goto invalid_key;

    char *key = keys(h);
    off_t key_len = keylen(key);
    if (url_len != key_len || strncmp(url, key, url_len)) goto invalid_key;
    char *val = key + key_len + 1;

    response = MHD_create_response_from_data(vallen(val), val, MHD_NO, MHD_NO);
    MHD_add_response_header (response, "Content-Type", "text/plain");
    ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return ret;

invalid_key:
    response = MHD_create_response_from_data(strlen(err), err, MHD_NO, MHD_NO);
    MHD_add_response_header (response, "Content-Type", "text/plain");
    ret = MHD_queue_response(connection, 404, response);
    MHD_destroy_response(response);
    return ret;
}

int core_count() {
#ifdef __APPLE__
    size_t len = 4;
    uint32_t count;
    int nm[2] = {CTL_HW, HW_AVAILCPU};
    sysctl(nm, 2, &count, &len, NULL, 0);
    if (count < 1) {
        nm[1] = HW_NCPU;
        sysctl(nm, 2, &count, &len, NULL, 0);
        if (count < 1) { count = 1; }
    }
    return count;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

int main(int argc, char **argv) {
    parse_opts(&argc,&argv);
    if (argc != 1) die("usage: %s\n", usage);
    const char *const data_file = argv[0];
    if (!index_file) {
        index_file = malloc(strlen(data_file) + 5);
        sprintf(index_file, "%s.idx", data_file);
    }
    else if (!strcmp(index_file, "-")) index_file = NULL;

    struct stat fs;
    data = mmap_file(data_file, &fs);

    int magic_len = strlen(magic) + 1;
    if (index_file) {
        FILE *f = fopen(index_file, "r");
        if (!f) {
            if (errno == ENOENT) goto build_index;
            die("Error opening \"%s\": %s\n", index_file, errstr);
        }
        fprintf(stderr, "Loading index file \"%s\"...\n", index_file);
        char magic_buf[magic_len];
        int r = fread(magic_buf, 1, magic_len, f);
        if (r != magic_len) {
            if (feof(f)) {
                die("Premature end of index file \"%s\"\n", index_file);
            } else {
                die("Error reading \"%s\": %s\n", index_file, errstr);
            }
        }
        if (strcmp(magic, magic_buf))
            die("Invalid index file: %s\n", index_file);
        hash = cmph_load(f);
        if (!hash) die("Error loading \"%s\": Corrupt CMPH data\n", index_file);
        n = cmph_size(hash);
        offsets = malloc(n*sizeof(off_t));
        r = fread(offsets, sizeof(off_t), n, f);
        if (r != n) {
            if (feof(f)) {
                die("Premature end of index file\n");
            } else {
                die("Error reading \"%s\": %s\n", index_file, errstr);
            }
        }
        if (fclose(f)) die("Error closing \"%s\": %s\n", index_file, errstr);
    } else {
        off_t allocated;
    build_index:
        fprintf(stderr, "Building index...\n");
        allocated = 4096;
        offsets = malloc(allocated*sizeof(off_t));
        char *p = data, *end = p + fs.st_size;

        for (;;) {
            if (allocated <= n) {
                allocated = (n+1)*fs.st_size/(p-data)+1;
                offsets = realloc(offsets, allocated*sizeof(off_t));
            }
            char *nl = memchr(p, '\n', end-p);
            if (!nl) break;
            char *tb = memchr(p, '\t', nl-p);
            if (tb) offsets[n++] = p - data;
            p = nl+1;
        }
        if (!n) die("File \"%s\" contains no key-value pairs\n", data_file);
        offsets = realloc(offsets, n*sizeof(off_t));

        off_t index;
        cmph_io_adapter_t adapter;
        adapter.data = (void*)&index;
        adapter.nkeys = n;
        adapter.read = key_read;
        adapter.rewind = key_rewind;
        adapter.dispose = key_dispose;

        // TODO: cmph segfaults on duplicate keys.
        // TODO: cmph segfaults on some binary data.
        cmph_config_t *config = cmph_config_new(&adapter);
        cmph_config_set_algo(config, CMPH_CHD);
        hash = cmph_new(config);
        cmph_config_destroy(config);
        if (!hash) die("error creating minimal perfect hash\n");

        off_t *offsets_by_hash = malloc(n*sizeof(off_t));
        int i; for (i = 0; i < n; i++) {
            char *key = keys(i);
            cmph_uint32 h = cmph_search(hash, key, keylen(key));
            offsets_by_hash[h] = key - data;
        }
        free(offsets);
        offsets = offsets_by_hash;

        if (index_file) {
            fprintf(stderr, "Saving index file \"%s\"...\n", index_file);
            FILE *f = fopen(index_file, "w");
            if (!f) die("Error opening \"%s\": %s\n", index_file, errstr);
            int w = fwrite(magic, 1, magic_len, f);
            if (w != magic_len) die("Error writing \"%s\": %s\n", index_file, errstr);
            cmph_dump(hash, f);
            w = fwrite(offsets, sizeof(off_t), n, f);
            if (w != n) die("Error writing \"%s\": %s\n", index_file, errstr);
            if (fclose(f)) die("Error closing \"%s\": %s\n", index_file, errstr);
        }
    }
    if (!serve_data) return 0;

    if (!threads) threads = core_count();
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
        &handle_request, NULL,
        MHD_OPTION_THREAD_POOL_SIZE, threads,
        MHD_OPTION_END
    );
    if (!daemon) die("Failed starting http server\n");
    fprintf(stderr, "Serving data @ %.3f ms...\n", ((double)1000*clock())/CLOCKS_PER_SEC);
    for (;;) sleep(0xffffffff);
    MHD_stop_daemon (daemon);

    return 0;
}
