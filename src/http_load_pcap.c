#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "fpcap_convert.h"
#include "http_load_pcap.h"

#define FHTTP_DEFAULT_QUEUE_SIZE 1
#define HTTP_MAX_ONE_LEN         2048

typedef struct data_pkg_t{
    struct timeval ts;
    char*  data;
    int    len;
} data_pkg_t;

typedef struct resp_t{
    pl_mgr      pkg_list;
    uint32_t    pkg_cnt;
    struct timeval cts; // timestamp of creation
} resp_t;

typedef struct pcap_state_t{
    pl_mgr   resp_list;
    uint32_t resp_cnt;
    struct timeval syn_ts; // timestamp of creation
} pcap_state_t;

struct _cli_state_t {
    pcap_state_t* state;
    liter         resp_iter;
    liter         pkg_iter;
    resp_t*       curr_resp;
    data_pkg_t*   curr_pkg;
};

cli_state_t* pc_create_state()
{
    cli_state_t* state = malloc(sizeof(cli_state_t));
    state->state = NULL;
    state->curr_resp = NULL;
    state->curr_pkg = NULL;
    return state;
}

typedef struct {
    pcap_state_t* state;
    resp_t* curr_resp;
    data_pkg_t* curr_pkg;
} sess_state_t;

static pcap_state_t** session_queue = NULL;
static int resp_queue_idx = 0;
static int resp_queue_size = 0;
static int resp_queue_max = 0;

void pc_destroy_state(cli_state_t* state)
{
    free(state);
}

void pc_get_next_session(cli_state_t* cli_state)
{
    if( resp_queue_idx == resp_queue_size ) {
        resp_queue_idx = 0;
    }

    //printf("current session idx=%d\n", resp_queue_idx);
    pcap_state_t* state = session_queue[resp_queue_idx++];
    cli_state->state = state;
    cli_state->resp_iter = flist_iter(state->resp_list);
    cli_state->curr_resp = NULL;
    cli_state->curr_pkg = NULL;
}

//void dump_pkgs(pcap_state_t* state)
//{
//    resp_t* resp = state->curr;
//    liter iter = flist_iter(resp->pkg_list);
//    data_pkg_t* pkg = NULL;
//    while( (pkg = flist_each(&iter)) ) {
//        printf("pkg:%s\n", pkg->data);
//    }
//}

void pc_get_next_resp(cli_state_t* cli_state)
{
    assert(!flist_isempty(cli_state->state->resp_list));
    pcap_state_t* state = cli_state->state;

    cli_state->curr_resp = flist_each(&cli_state->resp_iter);
    if( !cli_state->curr_resp ) {
        cli_state->resp_iter = flist_iter(state->resp_list);
        cli_state->curr_resp = flist_each(&cli_state->resp_iter);
    }

    // reset pkg iter
    resp_t* resp = cli_state->curr_resp;
    cli_state->pkg_iter = flist_iter(resp->pkg_list);
    cli_state->curr_pkg = NULL; // must set NULL, due to getlatency need it
    //dump_pkgs(state);
    if ( flist_isempty((pl_mgr)(&cli_state->pkg_iter)) ) {
        abort();
    }
}

char* pc_get_pkg_data(cli_state_t* cli_state)
{
    return cli_state->curr_pkg->data;
}

int  pc_get_pkg_len(cli_state_t* cli_state)
{
    return cli_state->curr_pkg->len;
}

void pc_get_next_pkg(cli_state_t* cli_state)
{
    cli_state->curr_pkg = flist_each(&cli_state->pkg_iter);
    if( !cli_state->curr_pkg ) {
        printf("get_next_pkg:pkg_list isempty=%d\n",
                flist_isempty((pl_mgr)(&cli_state->pkg_iter)));
        abort();
    }
}

int  pc_is_last_pkg(cli_state_t* cli_state)
{
    liter t_iter = cli_state->pkg_iter;
    data_pkg_t* pkg = flist_each(&t_iter);
    if( !pkg )
        return 1;

    return 0;
}

static
int ts_diff(struct timeval* ts1, struct timeval* ts2)
{
    int diff_sec = ts2->tv_sec - ts1->tv_sec;
    if ( diff_sec > 0 )
        return (diff_sec * 1000000 + ts2->tv_usec - ts1->tv_usec) / 1000;
    else
        return (ts2->tv_usec - ts1->tv_usec) / 1000;
}

static
int ts_equal(struct timeval* ts1, struct timeval* ts2)
{
    if( (ts1->tv_sec == ts2->tv_sec) &&
        (ts1->tv_usec == ts2->tv_usec) ) {
        return 1;
    }

    return 0;
}

int  pc_get_next_pkg_latency(cli_state_t* cli_state)
{
    resp_t* resp = cli_state->curr_resp;
    assert(resp);
    if( !resp ) {
        return FPCAP_NO_LATENCY;
    }

    data_pkg_t* curr_pkg = cli_state->curr_pkg;
    liter t_iter = cli_state->pkg_iter;
    data_pkg_t* next_pkg = flist_each(&t_iter);

    if( !next_pkg ) {
        return FPCAP_NO_LATENCY;
    }

    if( !curr_pkg ) {
        //printf("path1\n");
        // if it's not the first response, we need to consider the request latency
        int diff = ts_diff(&resp->cts, &next_pkg->ts);
        return ts_equal(&resp->cts, &cli_state->state->syn_ts) ? diff : diff/2;
    } else {
        //printf("path2\n");
        return ts_diff(&curr_pkg->ts, &next_pkg->ts);
    }
}

static
resp_t* create_resp(struct timeval* ts)
{
    resp_t* resp = malloc(sizeof(resp_t));
    resp->pkg_list = flist_create();
    resp->pkg_cnt = 0;
    resp->cts = *ts; // set the timestamp of the first pkg

    return resp;
}

static
data_pkg_t* create_pkg(struct timeval* ts, char* data, int len)
{
    data_pkg_t* pkg = malloc(sizeof(data_pkg_t));
    pkg->ts = *ts;
    pkg->data = malloc(len);
    memcpy(pkg->data, data, len);
    pkg->len = len;
    return pkg;
}

static
void destroy_pcap_resp(resp_t* resp)
{
    if( !resp ) return;
    data_pkg_t* pkg = NULL;
    while( (pkg = flist_pop(resp->pkg_list)) ) {
        free(pkg->data);
        free(pkg);
    }
    flist_delete(resp->pkg_list);
    free(resp);
}

static
void destroy_pcap_state(pcap_state_t* state)
{
    resp_t* resp = NULL;
    while( (resp = flist_pop(state->resp_list)) ) {
        printf("destroy resp\n");
        destroy_pcap_resp(resp);
    }

    printf("destroy curr resp\n");
    flist_delete(state->resp_list);
    free(state);
}

static
void create_session(session_t* sess, fapp_data_t* app_data)
{
    sess_state_t* sess_state = malloc(sizeof(sess_state_t));
    pcap_state_t* state = malloc(sizeof(pcap_state_t));
    state->resp_list = flist_create();
    state->resp_cnt = 0;
    state->syn_ts = app_data->ts;
    sess_state->state = state;
    sess_state->curr_resp = NULL;
    sess_state->curr_pkg = NULL;

    sess->ud = sess_state;
}

static
void destroy_session(session_t* sess)
{
    sess_state_t* sess_state = sess->ud;
    destroy_pcap_state(sess_state->state);
    destroy_pcap_resp(sess_state->curr_resp);
    free(sess_state);
}

static
const char* http_getline(const char* input, int total_len, int* offset, char* line, int len)
{
    int idx = 0;
    int cplen = 0;
    *offset = 0;
    memset(line, 0, len);
    //printf("------------------\ninput=%s\n------------\n", input);
    while(idx < total_len - 1) {
        if( input[idx] == '\n' ) {
            *offset = idx + 1;
            break;
        } else if( input[idx+1] == '\n' ) {
            *offset = idx + 2;
            break;
        } else {
            idx++;
        }
    }

    if( !(*offset) ) return NULL;

    cplen = idx;
    if( cplen == 0 || (len - 1 < cplen) ) {
        return NULL;
    }

    //printf("--- offset=%d, tol=%d, oplen=%d\n---------------\n", *offset, total_len, cplen);
    return strncpy(line, input, cplen);
}

//static
int simple_split(const char* str, char* left, char* right, char delim)
{
    int idx = 0;
    while( str[idx++] != delim );

    strncpy(left, str, idx-1);
    strcpy(right, str+idx);
    return 0;
}

static
int is_new_resp(const char* data, int len)
{
    char line[HTTP_MAX_ONE_LEN];
    int offset = 0;
    int tot_offset = 0;
    char http_version[10];
    char status[20];
    int return_code = 0;
    const char* tp = http_getline(data+tot_offset, len-tot_offset, &offset,
                                  line, HTTP_MAX_ONE_LEN);
    if( !tp ) {
        printf("this line is too long\n");
        return 0;
    }


    sscanf(line, "%s %d %s", http_version, &return_code, status);
    if( !strcasecmp(http_version, "HTTP/1.1") ||
        !strcasecmp(http_version, "HTTP/1.0") ) {
        return 1;
    } else {
        return 0;
    }
}

static
void assemble_response(sess_state_t* sess_state)
{
    if( !sess_state->curr_resp ) return;
    flist_push(sess_state->state->resp_list, sess_state->curr_resp);
    sess_state->state->resp_cnt++;
}

static
void assemble_package(sess_state_t* sess_state, resp_t* resp, data_pkg_t* pkg)
{
    flist_push(resp->pkg_list, pkg);
    resp->pkg_cnt++;
    sess_state->curr_pkg = pkg;
}

static
void process_data(session_t* sess, fapp_data_t* app_data, void* ud)
{
    sess_state_t* sess_state = sess->ud;
    pcap_state_t* state = sess_state->state;

    int new_resp = is_new_resp(app_data->data, app_data->len);
    if( new_resp ) {
        resp_t* old_resp = sess_state->curr_resp;
        if( old_resp ) {
            // push last resp into list
            assemble_response(sess_state);

            // use the last pkg's timestamp as syn_ts
            sess_state->curr_resp = create_resp(&(sess_state->curr_pkg->ts));
        } else {
            sess_state->curr_resp = create_resp(&state->syn_ts);
        }
    }

    resp_t* resp = sess_state->curr_resp;
    data_pkg_t* pkg = create_pkg(&app_data->ts, app_data->data, app_data->len);
    assemble_package(sess_state, resp, pkg);
}

static
void session_over(session_t* sess)
{
    sess_state_t* sess_state = sess->ud;
    pcap_state_t* state = sess_state->state;
    // finish the last resp assemble job
    assemble_response(sess_state);

    printf("session [%d]: build complete, contain %u responses\n", resp_queue_idx,
            state->resp_cnt);
    int resp_loop(void* data) {
        resp_t* resp = (resp_t*)data;
        printf(" \\_%u packages\n", resp->pkg_cnt);
        assert(resp->pkg_cnt);
        assert( !flist_isempty(resp->pkg_list) );
        return 0;
    }
    flist_foreach(state->resp_list, resp_loop);

    if( !flist_isempty(state->resp_list) ) {
        // no space
        if( resp_queue_idx == resp_queue_max ) {
            resp_queue_max = resp_queue_max * 2 + 1;
            session_queue = (pcap_state_t**)realloc(session_queue, sizeof(pcap_state_t*) * resp_queue_max);
        }

        // assemble to global queue
        session_queue[resp_queue_idx] = state;
        resp_queue_idx++;
        resp_queue_size++;
        free(sess_state);
    } else {
        destroy_session(sess);
    }
}

static
void loading_pkg(fsession_event event, session_t* sess, fapp_data_t* app_data, void* ud)
{
    switch(event) {
    case FSESSION_CREATE:
        //printf("create session\n");
        create_session(sess, app_data);
        break;
    case FSESSION_PROCESS:
        //printf("session processing:\n%s\n", app_data->data);
        process_data(sess, app_data, ud);
        break;
    case FSESSION_DELETE:
        //printf("session over\n");
        session_over(sess);
        break;
    default:
        printf("FATAL ERROR in loading pkg\n");
        exit(1);
    }
}

static
int cleanup_foreach(session_t* sess, void* ud)
{
    printf("cleanup....................................\n");
    destroy_session(sess);
    printf("cleanup....................................done\n");
    return 0;
}

int load_http_resp(const char* filename, const char* rules)
{
    // init session queue
    session_queue = (pcap_state_t**)malloc(sizeof(pcap_state_t*) * FHTTP_DEFAULT_QUEUE_SIZE);
    resp_queue_idx = 0;
    resp_queue_size = 0;
    resp_queue_max = FHTTP_DEFAULT_QUEUE_SIZE;

    convert_action_t action;
    action.pcap_filename = filename;
    action.filter_rules = rules;
    action.type = FPCAP_CONV_SERVER;
    action.handler = loading_pkg;
    action.cleanup = cleanup_foreach;
    action.ud = NULL;

    if( fpcap_convert(action) ) {
        return 1;
    }

    return 0;
}
