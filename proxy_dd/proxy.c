#include "proxy.h"
#include <unistd.h>
#include <strings.h>

#define DEFAULT_CONTAINER_PLATFORM "docker"
#define DEFAULT_QUEUE_DOCKER_IMAGE "single:queue"
#define DEFAULT_QUEUE_APPTAINER_IMAGE "../stages/single_queue.sif"
#define DEFAULT_TRACE_DOCKER_IMAGE "trace:generator"
#define DEFAULT_TRACE_GENERATOR_BINARY "../TRACE_GENERATOR/main"

/* NFR manager instances: one manager/worker pool per configured task in each stage pipeline. */
static struct nfr_manager nfr_managers_in[10][INPUT_TASKS];
static struct nfr_manager nfr_managers_out[10][OUTPUT_TASKS];
static struct nfr_manager application_managers[10];
static int nfr_initialized = 0;
/* link stats lock */
static pthread_mutex_t link_stats_lock = PTHREAD_MUTEX_INITIALIZER;
/* Global configuration pointer to inspect stage order for chaining */
static struct config *global_config = NULL;
/* outstanding jobs counter and sync */
static long outstanding_jobs = 0;
static pthread_mutex_t outstanding_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t outstanding_cond = PTHREAD_COND_INITIALIZER;

static char active_container_platform[32] = DEFAULT_CONTAINER_PLATFORM;
static char active_queue_container_image[512] = DEFAULT_QUEUE_DOCKER_IMAGE;
static char active_trace_container_image[512] = DEFAULT_TRACE_DOCKER_IMAGE;
static char active_trace_generator_binary[512] = DEFAULT_TRACE_GENERATOR_BINARY;

static void inc_outstanding()
{
    pthread_mutex_lock(&outstanding_lock);
    outstanding_jobs++;
    pthread_mutex_unlock(&outstanding_lock);
}

static void dec_outstanding()
{
    pthread_mutex_lock(&outstanding_lock);
    if (outstanding_jobs > 0) outstanding_jobs--;
    if (outstanding_jobs == 0)
        pthread_cond_broadcast(&outstanding_cond);
    pthread_mutex_unlock(&outstanding_lock);
}

int is_container_platform_name(const char *value)
{
    return value &&
           (strcasecmp(value, "docker") == 0 ||
            strcasecmp(value, "apptainer") == 0 ||
            strcasecmp(value, "singularity") == 0);
}

static int active_runtime_is_docker(void)
{
    return strcasecmp(active_container_platform, "docker") == 0;
}

static int active_runtime_is_apptainer(void)
{
    return strcasecmp(active_container_platform, "apptainer") == 0 ||
           strcasecmp(active_container_platform, "singularity") == 0;
}

static const char *active_runtime_command(void)
{
    if (strcasecmp(active_container_platform, "singularity") == 0)
        return "singularity";
    if (active_runtime_is_apptainer())
        return "apptainer";
    return "docker";
}

static void copy_config_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;

    dst[0] = '\0';
    if (!src)
        return;

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static char *shell_quote(const char *value)
{
    const char *text = value ? value : "";
    size_t size = 3;

    for (const char *p = text; *p; ++p)
        size += (*p == '\'') ? 4 : 1;

    char *quoted = malloc(size);
    if (!quoted)
        return NULL;

    char *out = quoted;
    *out++ = '\'';
    for (const char *p = text; *p; ++p)
    {
        if (*p == '\'')
        {
            memcpy(out, "'\\''", 4);
            out += 4;
        }
        else
        {
            *out++ = *p;
        }
    }
    *out++ = '\'';
    *out = '\0';
    return quoted;
}

void configure_container_runtime(struct config *configuration)
{
    if (!configuration)
        return;

    if (!is_container_platform_name(configuration->container_platform))
    {
        if (configuration->container_platform[0] != '\0')
            printf("Warning: unknown container_platform '%s'; using docker\n", configuration->container_platform);
        copy_config_string(configuration->container_platform, sizeof(configuration->container_platform), DEFAULT_CONTAINER_PLATFORM);
    }

    copy_config_string(active_container_platform, sizeof(active_container_platform), configuration->container_platform);

    if (active_runtime_is_apptainer() &&
        (configuration->queue_container_image[0] == '\0' ||
         strcmp(configuration->queue_container_image, DEFAULT_QUEUE_DOCKER_IMAGE) == 0))
    {
        copy_config_string(configuration->queue_container_image, sizeof(configuration->queue_container_image), DEFAULT_QUEUE_APPTAINER_IMAGE);
    }
    else if (active_runtime_is_docker() && configuration->queue_container_image[0] == '\0')
    {
        copy_config_string(configuration->queue_container_image, sizeof(configuration->queue_container_image), DEFAULT_QUEUE_DOCKER_IMAGE);
    }

    if (active_runtime_is_apptainer() &&
        strcmp(configuration->trace_container_image, DEFAULT_TRACE_DOCKER_IMAGE) == 0)
    {
        configuration->trace_container_image[0] = '\0';
    }
    else if (active_runtime_is_docker() && configuration->trace_container_image[0] == '\0')
    {
        copy_config_string(configuration->trace_container_image, sizeof(configuration->trace_container_image), DEFAULT_TRACE_DOCKER_IMAGE);
    }

    if (configuration->trace_generator_binary[0] == '\0')
        copy_config_string(configuration->trace_generator_binary, sizeof(configuration->trace_generator_binary), DEFAULT_TRACE_GENERATOR_BINARY);

    copy_config_string(active_queue_container_image, sizeof(active_queue_container_image), configuration->queue_container_image);
    copy_config_string(active_trace_container_image, sizeof(active_trace_container_image), configuration->trace_container_image);
    copy_config_string(active_trace_generator_binary, sizeof(active_trace_generator_binary), configuration->trace_generator_binary);

    printf("Container platform: %s; queue image: %s\n",
           active_container_platform,
           active_queue_container_image);
}

static const char *default_algorithm_for_type(const struct config *configuration, int type)
{
    if (!configuration)
        return "";

    switch (type)
    {
    case NFR_COMPRESS:
        return configuration->compression_algo;
    case NFR_HASH:
        return configuration->hashing_algo;
    case NFR_ENCRYPT:
        return configuration->ida_algo;
    default:
        return "";
    }
}

static const char *output_task_name_for_type(int type)
{
    switch (type)
    {
    case NFR_COMPRESS:
        return "compress";
    case NFR_ENCRYPT:
        return "encrypt";
    case NFR_HASH:
        return "hash_calculate";
    default:
        return "task";
    }
}

static const char *input_task_name_for_type(int type)
{
    switch (type)
    {
    case NFR_COMPRESS:
        return "uncompress";
    case NFR_ENCRYPT:
        return "unencrypt";
    case NFR_HASH:
        return "hash_verify";
    default:
        return "task";
    }
}

static int parse_requirement_type(const char *value)
{
    if (!value)
        return NFR_NONE;

    if (strcmp(value, "compress") == 0 || strcmp(value, "compression") == 0)
        return NFR_COMPRESS;
    if (strcmp(value, "encrypt") == 0 || strcmp(value, "cipher") == 0 || strcmp(value, "cipherer") == 0)
        return NFR_ENCRYPT;
    if (strcmp(value, "hash") == 0 || strcmp(value, "hashing") == 0 || strcmp(value, "integrity") == 0)
        return NFR_HASH;

    return NFR_NONE;
}

static void fill_requirement(struct nfr_requirement *req, int type, const char *algorithm, int is_input)
{
    if (!req)
        return;

    memset(req, 0, sizeof(*req));
    req->type = type;
    strncpy(req->task_name, is_input ? input_task_name_for_type(type) : output_task_name_for_type(type), sizeof(req->task_name) - 1);
    req->task_name[sizeof(req->task_name) - 1] = '\0';
    if (algorithm)
    {
        strncpy(req->algorithm, algorithm, sizeof(req->algorithm) - 1);
        req->algorithm[sizeof(req->algorithm) - 1] = '\0';
    }
}

static struct stage_definition *stage_definition_by_stage(struct config *configuration, int stage)
{
    if (!configuration)
        return NULL;

    for (int si = 0; si < configuration->stages_number; ++si)
    {
        if (configuration->stage_definitions[si].stage == stage)
            return &configuration->stage_definitions[si];
    }

    return NULL;
}

static struct stage_definition *global_stage_definition(int stage)
{
    return stage_definition_by_stage(global_config, stage);
}

static double stage_filesystem_bandwidth(int stage)
{
    double penalty = 1.0;
    if (global_config && global_config->workers > 1) {
        penalty = 1.0 + (global_config->concurrency_penalty * (global_config->workers - 1));
    }
    struct stage_definition *stage_def = global_stage_definition(stage);
    double val = 0.0;
    if (stage_def && stage_def->b_fs > 0.0)
        val = stage_def->b_fs;
    else
        val = global_config ? global_config->b_fs : 0.0;
    return val > 0.0 ? val / penalty : 0.0;
}

static double stage_filesystem_read_bandwidth(int stage)
{
    double penalty = 1.0;
    if (global_config && global_config->workers > 1) {
        penalty = 1.0 + (global_config->concurrency_penalty * (global_config->workers - 1));
    }
    struct stage_definition *stage_def = global_stage_definition(stage);
    double val = 0.0;
    if (stage_def && stage_def->b_fs_read > 0.0)
        val = stage_def->b_fs_read;
    else if (stage_def && stage_def->b_fs > 0.0)
        val = stage_def->b_fs;
    else if (global_config && global_config->b_fs_read > 0.0)
        val = global_config->b_fs_read;
    else
        val = global_config ? global_config->b_fs : 0.0;
    return val > 0.0 ? val / penalty : 0.0;
}

static double stage_filesystem_write_bandwidth(int stage)
{
    double penalty = 1.0;
    if (global_config && global_config->workers > 1) {
        penalty = 1.0 + (global_config->concurrency_penalty * (global_config->workers - 1));
    }
    struct stage_definition *stage_def = global_stage_definition(stage);
    double val = 0.0;
    if (stage_def && stage_def->b_fs_write > 0.0)
        val = stage_def->b_fs_write;
    else if (stage_def && stage_def->b_fs > 0.0)
        val = stage_def->b_fs;
    else if (global_config && global_config->b_fs_write > 0.0)
        val = global_config->b_fs_write;
    else
        val = global_config ? global_config->b_fs : 0.0;
    return val > 0.0 ? val / penalty : 0.0;
}

static double stage_mean_interarrival_seconds(int stage, const struct worker *w)
{
    struct stage_definition *stage_def = global_stage_definition(stage);
    if (stage_def && stage_def->mean_interarrival > 0.0)
        return stage_def->mean_interarrival;
    if (w && w->sizeWorker > 0)
        return w->trace[0].mean_interarrival;
    return 0.0;
}

static int stage_uses_burst_arrivals(int stage)
{
    struct stage_definition *stage_def = global_stage_definition(stage);
    return stage_def ? stage_def->burst_arrival_mode : 0;
}

static double worker_recorded_time(struct worker *w)
{
    double total = 0.0;
    if (!w)
        return 0.0;

    for (int j = 0; j < w->sizeWorker; ++j)
    {
        total += w->trace[j].service_time_c;
        total += w->trace[j].service_time_h;
        total += w->trace[j].service_time_idx;
        total += w->trace[j].service_time_ida;
        total += w->trace[j].service_time_io;
        total += w->trace[j].service_time_app;
    }

    return total;
}

static void add_worker_stage_processing_time(struct worker *w, int stage, int is_input, double seconds)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES || seconds <= 0.0)
        return;

    if (is_input)
        w->stage_input_time[idx] += seconds;
    else
        w->stage_output_time[idx] += seconds;
}

static void add_worker_stage_application_time(struct worker *w, int stage, double seconds)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES || seconds <= 0.0)
        return;

    w->stage_application_time[idx] += seconds;
}

static void add_worker_stage_nfr_time(struct worker *w, int stage, int task_type, int is_input, double seconds)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES || task_type <= NFR_NONE || task_type >= NFR_COUNT || seconds <= 0.0)
        return;

    if (is_input)
        w->stage_nfr_input_time[idx][task_type] = fmax(w->stage_nfr_input_time[idx][task_type], seconds);
    else
        w->stage_nfr_output_time[idx][task_type] = fmax(w->stage_nfr_output_time[idx][task_type], seconds);
}

static void add_worker_stage_requirement_metrics(struct worker *w, int stage, int task_id, int is_input, double seconds, long input_size, long output_size)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES || task_id < 0 || task_id >= MAX_PIPELINE_TASKS)
        return;

    if (is_input)
    {
        if (seconds > 0.0)
            w->stage_input_requirement_time[idx][task_id] = fmax(w->stage_input_requirement_time[idx][task_id], seconds);
        w->stage_input_requirement_input_size[idx][task_id] = input_size;
        w->stage_input_requirement_output_size[idx][task_id] = output_size;
    }
    else
    {
        if (seconds > 0.0)
            w->stage_output_requirement_time[idx][task_id] = fmax(w->stage_output_requirement_time[idx][task_id], seconds);
        w->stage_output_requirement_input_size[idx][task_id] = input_size;
        w->stage_output_requirement_output_size[idx][task_id] = output_size;
    }
}

static void add_worker_stage_transfer_time(struct worker *w, int stage, double seconds)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES || seconds <= 0.0)
        return;

    w->stage_transfer_time[idx] += seconds;
}

static void record_worker_stage_input_size(struct worker *w, int stage)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES)
        return;

    w->stage_input_size[idx] = w->sizeStorage;
}

static void record_worker_stage_output_size(struct worker *w, int stage)
{
    int idx = stage - 1;
    if (!w || idx < 0 || idx >= MAX_STAGES)
        return;

    w->stage_output_size[idx] = w->sizeStorage;
    w->output_workload_size = w->sizeStorage;
}

/* Compute network transfer time (seconds) for transferring worker's data from machine `from_mid` to `to_mid`.
   Returns 0.0 if same machine or no data to transfer. Looks up direct link in configuration->links.
*/
static double compute_network_transfer_time(struct worker *w, int from_mid, int to_mid)
{
    if (!w || from_mid == to_mid) return 0.0;
    if (!global_config) return 0.0;

    /* determine total bytes to transfer */
    double total_bytes = (double)w->sizeStorage;
    if (total_bytes <= 0.0) return 0.0;

    const char *from_name = "";
    const char *to_name = "";
    if (from_mid >= 0 && from_mid < global_config->machines_number) from_name = global_config->machines[from_mid].name;
    if (to_mid >= 0 && to_mid < global_config->machines_number) to_name = global_config->machines[to_mid].name;

    /* search for a link matching from->to (directional). If not found, try reverse direction. */
    double b_net = 0.0; /* bytes/sec */
    double latency_ms = 0.0;

    /* search for a link matching from->to (directional). If not found, try reverse direction. */
    for (int li = 0; li < global_config->links_number; ++li)
    {
        if (strcmp(global_config->links[li].from, from_name) == 0 && strcmp(global_config->links[li].to, to_name) == 0)
        {
            b_net = global_config->links[li].b_net;
            latency_ms = global_config->links[li].latency_ms;
            break;
        }
    }
    if (b_net <= 0.0)
    {
        /* try reverse link */
        for (int li = 0; li < global_config->links_number; ++li)
        {
            if (strcmp(global_config->links[li].from, to_name) == 0 && strcmp(global_config->links[li].to, from_name) == 0)
            {
                b_net = global_config->links[li].b_net;
                latency_ms = global_config->links[li].latency_ms;
                break;
            }
        }
    }

    if (b_net <= 0.0)
    {
        /* fallback to conservative default 10 MB/s */
        b_net = 10.0 * 1048576.0;
        latency_ms = 0.0;
    }

    double t_transfer = total_bytes / b_net; /* seconds */
    double t_latency = latency_ms / 1000.0;
    return t_transfer + t_latency;
}

static int parse_stage_identifier(const char *value)
{
    if (!value || value[0] == '\0')
        return 0;

    if (strncmp(value, "stage", 5) == 0)
        return atoi(value + 5);

    char *endptr = NULL;
    long numeric_id = strtol(value, &endptr, 10);
    if (endptr != value && *endptr == '\0' && numeric_id > 0 && numeric_id <= 10)
        return (int)numeric_id;

    /* Backward compatibility with older configs where operation names were used as stage names. */
    if (strcmp(value, "compress") == 0)
        return 1;
    if (strcmp(value, "hashing") == 0)
        return 2;
    if (strcmp(value, "indexing") == 0)
        return 3;
    if (strcmp(value, "dispersal") == 0)
        return 4;
    if (strcmp(value, "upload") == 0)
        return 5;

    return 0;
}

static void init_stage_definition(struct config *configuration, int index, int stage, const char *name)
{
    struct stage_definition *stage_def = &configuration->stage_definitions[index];
    memset(stage_def, 0, sizeof(*stage_def));
    stage_def->stage = stage;
    stage_def->mean_interarrival = 0.0;
    stage_def->burst_arrival_mode = 0;
    stage_def->b_fs = configuration ? configuration->b_fs : 0.0;
    stage_def->b_fs_read = configuration ? configuration->b_fs_read : stage_def->b_fs;
    stage_def->b_fs_write = configuration ? configuration->b_fs_write : stage_def->b_fs;
    stage_def->application_mean_service_time = configuration ? configuration->application_mean_service_time : 0.0;
    stage_def->application_size_factor = 1.0;
    if (name && name[0] != '\0')
    {
        strncpy(stage_def->name, name, sizeof(stage_def->name) - 1);
        stage_def->name[sizeof(stage_def->name) - 1] = '\0';
    }
    else
    {
        snprintf(stage_def->name, sizeof(stage_def->name), "stage%d", stage);
    }
}

static cJSON *first_number_item(cJSON *object, const char **keys, int key_count)
{
    if (!object || !keys)
        return NULL;

    for (int i = 0; i < key_count; ++i)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(object, keys[i]);
        if (cJSON_IsNumber(item))
            return item;
    }

    return NULL;
}

static cJSON *first_string_item(cJSON *object, const char **keys, int key_count)
{
    if (!object || !keys)
        return NULL;

    for (int i = 0; i < key_count; ++i)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(object, keys[i]);
        if (cJSON_IsString(item) && item->valuestring)
            return item;
    }

    return NULL;
}

static void parse_stage_filesystem_bandwidth(cJSON *stage, struct stage_definition *stage_def)
{
    if (!stage || !stage_def)
        return;

    cJSON *bfs = cJSON_GetObjectItemCaseSensitive(stage, "b_fs");
    if (cJSON_IsNumber(bfs))
    {
        /* Stage-level b_fs follows the global config convention: MB/s. */
        stage_def->b_fs = bfs->valuedouble * 1048576.0;
        stage_def->b_fs_read = stage_def->b_fs;
        stage_def->b_fs_write = stage_def->b_fs;
        return;
    }

    cJSON *bfs_bytes = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_bytes");
    if (!cJSON_IsNumber(bfs_bytes))
        bfs_bytes = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_bytes_per_sec");
    if (cJSON_IsNumber(bfs_bytes))
    {
        stage_def->b_fs = bfs_bytes->valuedouble;
        stage_def->b_fs_read = stage_def->b_fs;
        stage_def->b_fs_write = stage_def->b_fs;
    }

    cJSON *bfs_read = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_read");
    if (cJSON_IsNumber(bfs_read))
        stage_def->b_fs_read = bfs_read->valuedouble * 1048576.0;
    cJSON *bfs_write = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_write");
    if (cJSON_IsNumber(bfs_write))
        stage_def->b_fs_write = bfs_write->valuedouble * 1048576.0;

    cJSON *bfs_read_bytes = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_read_bytes");
    if (!cJSON_IsNumber(bfs_read_bytes))
        bfs_read_bytes = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_read_bytes_per_sec");
    if (cJSON_IsNumber(bfs_read_bytes))
        stage_def->b_fs_read = bfs_read_bytes->valuedouble;

    cJSON *bfs_write_bytes = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_write_bytes");
    if (!cJSON_IsNumber(bfs_write_bytes))
        bfs_write_bytes = cJSON_GetObjectItemCaseSensitive(stage, "b_fs_write_bytes_per_sec");
    if (cJSON_IsNumber(bfs_write_bytes))
        stage_def->b_fs_write = bfs_write_bytes->valuedouble;

    if (stage_def->b_fs_read <= 0.0)
        stage_def->b_fs_read = stage_def->b_fs;
    if (stage_def->b_fs_write <= 0.0)
        stage_def->b_fs_write = stage_def->b_fs;
}

static void parse_stage_mean_interarrival(cJSON *stage, struct stage_definition *stage_def)
{
    static const char *keys[] = {
        "inter_arrival",
        "mean_interarrival",
        "stage_interarrival",
        "stage_mean_interarrival"
    };

    cJSON *interarrival = first_number_item(stage, keys, sizeof(keys) / sizeof(keys[0]));
    if (stage_def && interarrival && interarrival->valuedouble > 0.0)
        stage_def->mean_interarrival = interarrival->valuedouble;
}

static void parse_stage_arrival_mode(cJSON *stage, struct stage_definition *stage_def)
{
    if (!stage || !stage_def)
        return;

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(stage, "arrival_model");
    if (!cJSON_IsString(mode))
        mode = cJSON_GetObjectItemCaseSensitive(stage, "queue_arrival_model");
    if (cJSON_IsString(mode) && mode->valuestring)
    {
        if (strcmp(mode->valuestring, "burst") == 0)
            stage_def->burst_arrival_mode = 1;
        return;
    }

    cJSON *burst = cJSON_GetObjectItemCaseSensitive(stage, "burst_arrival");
    if (cJSON_IsBool(burst))
        stage_def->burst_arrival_mode = cJSON_IsTrue(burst) ? 1 : 0;
}

static void parse_stage_application_time(cJSON *stage, struct stage_definition *stage_def)
{
    static const char *keys[] = {
        "application_mean_service_time",
        "application_mean_execution_time",
        "application_avg_execution_time",
        "application_execution_time"
    };

    cJSON *app_time = first_number_item(stage, keys, sizeof(keys) / sizeof(keys[0]));
    if (app_time && stage_def)
        stage_def->application_mean_service_time = app_time->valuedouble;
}

static void parse_stage_application_size_factor(cJSON *stage, struct stage_definition *stage_def)
{
    static const char *keys[] = {
        "application_size_factor",
        "application_transformation_factor",
        "application_output_size_factor",
        "application_data_size_factor"
    };

    cJSON *factor = first_number_item(stage, keys, sizeof(keys) / sizeof(keys[0]));
    if (!stage_def)
        return;

    stage_def->application_size_factor = (factor && factor->valuedouble > 0.0) ? factor->valuedouble : 1.0;
}

static int stage_has_application(const struct stage_definition *stage_def)
{
    return stage_def && stage_def->application_mean_service_time > 0.0;
}

static int add_requirement_to_pipeline(struct config *configuration, struct nfr_requirement *pipeline, int *count, int type, const char *algorithm, int is_input)
{
    if (!pipeline || !count || *count >= MAX_PIPELINE_TASKS || type == NFR_NONE)
        return -1;

    const char *selected_algorithm = (algorithm && algorithm[0]) ? algorithm : default_algorithm_for_type(configuration, type);
    fill_requirement(&pipeline[*count], type, selected_algorithm, is_input);
    (*count)++;
    return 0;
}

static void add_default_output_requirements(struct config *configuration, struct stage_definition *stage_def)
{
    if (!configuration || !stage_def || stage_def->output_count > 0)
        return;

    add_requirement_to_pipeline(configuration, stage_def->output_requirements, &stage_def->output_count, NFR_COMPRESS, NULL, 0);
    add_requirement_to_pipeline(configuration, stage_def->output_requirements, &stage_def->output_count, NFR_ENCRYPT, NULL, 0);
    add_requirement_to_pipeline(configuration, stage_def->output_requirements, &stage_def->output_count, NFR_HASH, NULL, 0);
}

static void parse_requirement_item(struct config *configuration, cJSON *item, struct stage_definition *stage_def, int is_input)
{
    int type = NFR_NONE;
    const char *algorithm = NULL;

    if (!item || !stage_def)
        return;

    if (cJSON_IsString(item) && item->valuestring)
    {
        type = parse_requirement_type(item->valuestring);
    }
    else if (cJSON_IsObject(item))
    {
        cJSON *type_item = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(type_item))
            type_item = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(type_item))
            type_item = cJSON_GetObjectItemCaseSensitive(item, "requirement");

        cJSON *algo_item = cJSON_GetObjectItemCaseSensitive(item, "algorithm");
        if (!cJSON_IsString(algo_item))
            algo_item = cJSON_GetObjectItemCaseSensitive(item, "algo");
        if (!cJSON_IsString(algo_item))
            algo_item = cJSON_GetObjectItemCaseSensitive(item, "cipherer");

        if (cJSON_IsString(type_item) && type_item->valuestring)
            type = parse_requirement_type(type_item->valuestring);
        else if (cJSON_IsString(algo_item) && algo_item->valuestring)
            type = NFR_ENCRYPT;

        if (cJSON_IsString(algo_item) && algo_item->valuestring)
            algorithm = algo_item->valuestring;
    }

    if (is_input)
        add_requirement_to_pipeline(configuration, stage_def->input_requirements, &stage_def->input_count, type, algorithm, 1);
    else
        add_requirement_to_pipeline(configuration, stage_def->output_requirements, &stage_def->output_count, type, algorithm, 0);
}

static int parse_requirements_array(struct config *configuration, cJSON *array, struct stage_definition *stage_def, int is_input)
{
    if (!cJSON_IsArray(array) || !stage_def)
        return 0;

    if (is_input)
    {
        stage_def->input_count = 0;
        stage_def->input_explicit = 1;
    }
    else
    {
        stage_def->output_count = 0;
    }

    cJSON *item;
    cJSON_ArrayForEach(item, array)
    {
        parse_requirement_item(configuration, item, stage_def, is_input);
    }

    return 1;
}

static void mirror_output_to_input(struct config *configuration, struct stage_definition *source, struct stage_definition *target)
{
    if (!configuration || !source || !target)
        return;

    target->input_count = 0;
    for (int out = source->output_count - 1; out >= 0; --out)
    {
        struct nfr_requirement *req = &source->output_requirements[out];
        add_requirement_to_pipeline(configuration, target->input_requirements, &target->input_count, req->type, req->algorithm, 1);
    }
}

static void finalize_stage_pipelines(struct config *configuration)
{
    if (!configuration)
        return;

    for (int si = 0; si < configuration->stages_number; ++si)
    {
        struct stage_definition *stage_def = &configuration->stage_definitions[si];
        if (si == 0)
        {
            if (!stage_def->input_explicit)
                mirror_output_to_input(configuration, stage_def, stage_def);
        }
        else
        {
            mirror_output_to_input(configuration, &configuration->stage_definitions[si - 1], stage_def);
        }
    }
}

static int configured_stage_position(int stage)
{
    if (!global_config)
        return -1;

    for (int si = 0; si < global_config->stages_number; ++si)
    {
        if (global_config->stages[si] == stage)
            return si;
    }

    return -1;
}

static int stage_machine_id(int stage)
{
    if (!global_config)
        return -1;

    for (int mid = 0; mid < global_config->machines_number; ++mid)
    {
        for (int s = 0; s < global_config->machines[mid].stages_number; ++s)
        {
            if (global_config->machines[mid].stages[s] == stage)
                return mid;
        }
    }

    return -1;
}

static int machine_service_profile_index(int machine_id)
{
    if (!global_config)
        return 0;
    if (machine_id < 0 || machine_id >= global_config->machines_number)
        return 0;
    return global_config->machines[machine_id].service_profile_index;
}

static void set_worker_task_context(struct worker *w, const struct nfr_manager *m)
{
    if (!w || !m)
        return;

    w->stage = m->stage;
    w->stage_owner = m->stage;
    w->pipeline_is_input = (m->is_application || !m->is_input) ? 0 : 1;
    w->task_id = m->task_id;
    w->task_type = m->task_type;
    strncpy(w->task_name, m->task_name, sizeof(w->task_name) - 1);
    w->task_name[sizeof(w->task_name) - 1] = '\0';
    strncpy(w->task_algorithm, m->task_algorithm, sizeof(w->task_algorithm) - 1);
    w->task_algorithm[sizeof(w->task_algorithm) - 1] = '\0';
    strncpy(w->agent_type, m->is_application ? "application" : (m->is_input ? "input" : "output"), sizeof(w->agent_type) - 1);
    w->agent_type[sizeof(w->agent_type) - 1] = '\0';
    w->b_fs = stage_filesystem_bandwidth(m->stage);
    w->b_fs_read = stage_filesystem_read_bandwidth(m->stage);
    w->b_fs_write = stage_filesystem_write_bandwidth(m->stage);

    int mid = stage_machine_id(m->stage);
    w->machine_id = mid;
    w->service_profile_index = machine_service_profile_index(mid);
}

static void advance_to_next_stage_or_finish(int current_stage, struct worker *w);
static int enqueue_stage_start(int stage, struct worker *w);
static double application_time(struct worker *my_data);

static int enqueue_pipeline_task(int stage, int is_input, int task_id, struct worker *w)
{
    int idx = stage - 1;
    struct stage_definition *stage_def = global_stage_definition(stage);
    if (idx < 0 || idx >= 10 || !w)
        return -1;

    if (is_input)
    {
        if (!stage_def || task_id < 0 || task_id >= stage_def->input_count || nfr_managers_in[idx][task_id].threads == NULL)
            return -1;
        return nfr_manager_enqueue(&nfr_managers_in[idx][task_id], w);
    }

    if (!stage_def || task_id < 0 || task_id >= stage_def->output_count || nfr_managers_out[idx][task_id].threads == NULL)
        return -1;
    return nfr_manager_enqueue(&nfr_managers_out[idx][task_id], w);
}

static int enqueue_application_task(int stage, struct worker *w)
{
    int idx = stage - 1;
    struct stage_definition *stage_def = global_stage_definition(stage);
    if (idx < 0 || idx >= 10 || !w || !stage_has_application(stage_def) || application_managers[idx].threads == NULL)
        return -1;

    return nfr_manager_enqueue(&application_managers[idx], w);
}

static int enqueue_application_or_output(int stage, struct worker *w)
{
    struct stage_definition *stage_def = global_stage_definition(stage);
    if (!stage_def || !w)
        return -1;

    if (stage_has_application(stage_def))
        return enqueue_application_task(stage, w);

    if (stage_def->output_count > 0)
        return enqueue_pipeline_task(stage, 0, 0, w);

    advance_to_next_stage_or_finish(stage, w);
    return 0;
}

static void advance_to_next_stage_or_finish(int current_stage, struct worker *w)
{
    if (!global_config || !w)
    {
        dec_outstanding();
        return;
    }

    record_worker_stage_output_size(w, current_stage);

    int found = configured_stage_position(current_stage);
    if (found < 0 || found + 1 >= global_config->stages_number)
    {
        dec_outstanding();
        return;
    }

    int next_stage = global_config->stages[found + 1];
    int from_mid = w->machine_id;
    int to_mid = stage_machine_id(next_stage);

    double net_t = compute_network_transfer_time(w, from_mid, to_mid);
    if (net_t > 0.0)
    {
        add_worker_stage_transfer_time(w, current_stage, net_t);
        double total = (double)w->sizeStorage;
        if (total > 0.0)
        {
            for (int tj = 0; tj < w->sizeWorker; ++tj)
            {
                double frac = (double)w->trace[tj].size / total;
                w->trace[tj].service_time_io += (float)(net_t * frac);
            }
        }

        if (from_mid >= 0 && to_mid >= 0 && from_mid < global_config->machines_number && to_mid < global_config->machines_number)
        {
            const char *from_name = global_config->machines[from_mid].name;
            const char *to_name = global_config->machines[to_mid].name;
            pthread_mutex_lock(&link_stats_lock);
            for (int li = 0; li < global_config->links_number; ++li)
            {
                if ((strcmp(global_config->links[li].from, from_name) == 0 && strcmp(global_config->links[li].to, to_name) == 0) ||
                    (strcmp(global_config->links[li].from, to_name) == 0 && strcmp(global_config->links[li].to, from_name) == 0))
                {
                    global_config->links[li].bytes_transferred += (double)w->sizeStorage;
                    global_config->links[li].transfers_count += 1;
                    global_config->links[li].total_transfer_time += net_t;
                    break;
                }
            }
            pthread_mutex_unlock(&link_stats_lock);
        }

        //usleep((useconds_t)(net_t * 1e6));
    }

    w->machine_id = to_mid;
    w->service_profile_index = machine_service_profile_index(to_mid);
    w->stage_owner = next_stage;

    if (enqueue_stage_start(next_stage, w) != 0)
        dec_outstanding();
}

static int enqueue_stage_start(int stage, struct worker *w)
{
    struct stage_definition *stage_def = global_stage_definition(stage);
    if (!stage_def || !w)
        return -1;

    w->b_fs = stage_def->b_fs;
    w->b_fs_read = stage_def->b_fs_read;
    w->b_fs_write = stage_def->b_fs_write;
    record_worker_stage_input_size(w, stage);

    if (stage_def->input_count > 0)
        return enqueue_pipeline_task(stage, 1, 0, w);

    return enqueue_application_or_output(stage, w);
}

static void *nfr_worker_thread(void *arg)
{
    struct nfr_manager *m = (struct nfr_manager *)arg;

    while (1)
    {
        pthread_mutex_lock(&m->lock);
        while (m->q_count == 0 && !m->stop)
            pthread_cond_wait(&m->cond_nonempty, &m->lock);

        if (m->stop && m->q_count == 0)
        {
            pthread_mutex_unlock(&m->lock);
            break;
        }

        struct nfr_job job = m->queue[m->q_head];
        m->q_head = (m->q_head + 1) % m->q_size;
        m->q_count--;
        pthread_cond_signal(&m->cond_nonfull);
        pthread_mutex_unlock(&m->lock);

        if (!job.w)
            continue;

        set_worker_task_context(job.w, m);

        struct timespec t0, t1;
        long requirement_input_size = job.w->sizeStorage;
        double recorded_before = worker_recorded_time(job.w);
        double simulated_delta = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (m->is_application)
        {
            simulated_delta = application_time(job.w);
        }
        else
        {
            serviceTime(job.w);
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long requirement_output_size = job.w->sizeStorage;
        if (!m->is_application)
        {
            double recorded_after = worker_recorded_time(job.w);
            simulated_delta = recorded_after - recorded_before;
            add_worker_stage_processing_time(job.w, m->stage, m->is_input, simulated_delta);
            add_worker_stage_nfr_time(job.w, m->stage, m->task_type, m->is_input, simulated_delta);
            add_worker_stage_requirement_metrics(job.w, m->stage, m->task_id, m->is_input, simulated_delta, requirement_input_size, requirement_output_size);
        }
        else
        {
            add_worker_stage_application_time(job.w, m->stage, simulated_delta);
        }
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

        pthread_mutex_lock(&m->lock);
        m->jobs_processed += 1;
        m->total_processing_time += elapsed;
        m->total_simulated_time += simulated_delta;
        pthread_mutex_unlock(&m->lock);

        if (m->is_application)
        {
            struct stage_definition *stage_def = global_stage_definition(m->stage);
            int output_count = stage_def ? stage_def->output_count : 0;

            if (output_count > 0)
            {
                if (enqueue_pipeline_task(m->stage, 0, 0, job.w) != 0)
                    dec_outstanding();
            }
            else
            {
                advance_to_next_stage_or_finish(m->stage, job.w);
            }
        }
        else if (m->is_input)
        {
            struct stage_definition *stage_def = global_stage_definition(m->stage);
            int input_count = stage_def ? stage_def->input_count : 0;

            if (m->task_id + 1 < input_count)
            {
                if (enqueue_pipeline_task(m->stage, 1, m->task_id + 1, job.w) != 0)
                    dec_outstanding();
            }
            else
            {
                if (enqueue_application_or_output(m->stage, job.w) != 0)
                    dec_outstanding();
            }
        }
        else
        {
            struct stage_definition *stage_def = global_stage_definition(m->stage);
            int output_count = stage_def ? stage_def->output_count : 0;

            if (m->task_id + 1 < output_count)
            {
                if (enqueue_pipeline_task(m->stage, 0, m->task_id + 1, job.w) != 0)
                    dec_outstanding();
            }
            else
            {
                advance_to_next_stage_or_finish(m->stage, job.w);
            }
        }
    }

    return NULL;
}

int nfr_manager_init(struct nfr_manager *m, int stage, int task_id, int task_type, const char *task_name, const char *task_algorithm, int num_threads, int q_size, int is_input, int is_application)
{
    m->stage = stage;
    m->task_id = task_id;
    m->task_type = task_type;
    strncpy(m->task_name, task_name ? task_name : "task", sizeof(m->task_name) - 1);
    m->task_name[sizeof(m->task_name) - 1] = '\0';
    strncpy(m->task_algorithm, task_algorithm ? task_algorithm : "", sizeof(m->task_algorithm) - 1);
    m->task_algorithm[sizeof(m->task_algorithm) - 1] = '\0';
    m->num_threads = num_threads > 0 ? num_threads : 1;
    m->q_size = q_size > 0 ? q_size : 1024;
    m->queue = malloc(sizeof(struct nfr_job) * m->q_size);
    if (!m->queue) return -1;
    m->q_head = m->q_tail = m->q_count = 0;
    m->stop = 0;
    m->is_input = is_input ? 1 : 0;
    m->is_application = is_application ? 1 : 0;
    m->jobs_processed = 0;
    m->total_processing_time = 0.0;
    m->total_simulated_time = 0.0;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->cond_nonempty, NULL);
    pthread_cond_init(&m->cond_nonfull, NULL);
    m->threads = malloc(sizeof(pthread_t) * m->num_threads);
    if (!m->threads) { free(m->queue); return -1; }

    for (int i = 0; i < m->num_threads; ++i)
        pthread_create(&m->threads[i], NULL, nfr_worker_thread, (void *)m);

    return 0;
}

int nfr_manager_enqueue(struct nfr_manager *m, struct worker *w)
{
    if (!m || !w) return -1;
    printf("[ENQUEUE] Stage %d %s task %d (%s:%s) worker %d\n",
           m->stage,
           m->is_application ? "APP" : (m->is_input ? "IN" : "OUT"),
           m->task_id,
           m->task_name,
           m->task_algorithm,
           w->id);
    pthread_mutex_lock(&m->lock);
    while (m->q_count == m->q_size && !m->stop)
        pthread_cond_wait(&m->cond_nonfull, &m->lock);
    if (m->stop)
    {
        pthread_mutex_unlock(&m->lock);
        return -1;
    }
    m->queue[m->q_tail].w = w;
    m->q_tail = (m->q_tail + 1) % m->q_size;
    m->q_count++;
    pthread_cond_signal(&m->cond_nonempty);
    pthread_mutex_unlock(&m->lock);
    return 0;
}

void nfr_manager_shutdown(struct nfr_manager *m)
{
    pthread_mutex_lock(&m->lock);
    m->stop = 1;
    pthread_cond_broadcast(&m->cond_nonempty);
    pthread_cond_broadcast(&m->cond_nonfull);
    pthread_mutex_unlock(&m->lock);

    for (int i = 0; i < m->num_threads; ++i)
        pthread_join(m->threads[i], NULL);

    free(m->threads);
    free(m->queue);
    m->threads = NULL;
    m->queue = NULL;
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->cond_nonempty);
    pthread_cond_destroy(&m->cond_nonfull);
}

static FILE *open_report_csv(const char *file_name)
{
    char path[256];
    mkdir("results", 0777);
    snprintf(path, sizeof(path), "results/%s", file_name);
    FILE *fp = fopen(path, "w");
    if (!fp)
        printf("Warning: could not open %s for writing\n", path);
    return fp;
}

void shutdown_and_report_metrics(struct config *configuration)
{
    printf("\nShutting down NFR managers...\n");
    if (!configuration) return;

    /* First: request stop on all managers and wake them so they don't enqueue to freed queues. */
    for (int si = 0; si < configuration->stages_number; ++si)
    {
        int sn = configuration->stages[si] - 1;
        struct stage_definition *stage_def = &configuration->stage_definitions[si];
        if (sn >= 0 && sn < 10)
        {
            for (int task = 0; task < stage_def->input_count; ++task)
            {
                if (nfr_managers_in[sn][task].threads)
                {
                    pthread_mutex_lock(&nfr_managers_in[sn][task].lock);
                    nfr_managers_in[sn][task].stop = 1;
                    pthread_cond_broadcast(&nfr_managers_in[sn][task].cond_nonempty);
                    pthread_cond_broadcast(&nfr_managers_in[sn][task].cond_nonfull);
                    pthread_mutex_unlock(&nfr_managers_in[sn][task].lock);
                }
            }
            if (stage_has_application(stage_def))
            {
                pthread_mutex_lock(&application_managers[sn].lock);
                application_managers[sn].stop = 1;
                pthread_cond_broadcast(&application_managers[sn].cond_nonempty);
                pthread_cond_broadcast(&application_managers[sn].cond_nonfull);
                pthread_mutex_unlock(&application_managers[sn].lock);
            }
            for (int task = 0; task < stage_def->output_count; ++task)
            {
                if (nfr_managers_out[sn][task].threads)
                {
                    pthread_mutex_lock(&nfr_managers_out[sn][task].lock);
                    nfr_managers_out[sn][task].stop = 1;
                    pthread_cond_broadcast(&nfr_managers_out[sn][task].cond_nonempty);
                    pthread_cond_broadcast(&nfr_managers_out[sn][task].cond_nonfull);
                    pthread_mutex_unlock(&nfr_managers_out[sn][task].lock);
                }
            }
        }
    }

    /* Second: join threads and free resources for all managers. */
    for (int si = 0; si < configuration->stages_number; ++si)
    {
        int sn = configuration->stages[si] - 1;
        struct stage_definition *stage_def = &configuration->stage_definitions[si];
        if (sn >= 0 && sn < 10)
        {
            for (int task = 0; task < stage_def->input_count; ++task)
            {
                if (nfr_managers_in[sn][task].threads)
                    nfr_manager_shutdown(&nfr_managers_in[sn][task]);
            }
            if (stage_has_application(stage_def))
                nfr_manager_shutdown(&application_managers[sn]);
            for (int task = 0; task < stage_def->output_count; ++task)
            {
                if (nfr_managers_out[sn][task].threads)
                    nfr_manager_shutdown(&nfr_managers_out[sn][task]);
            }
        }
    }

    printf("\n=== Manager Metrics ===\n");
    FILE *manager_csv = open_report_csv("manager_metrics.csv");
    if (manager_csv)
        fprintf(manager_csv, "stage,stage_name,pipeline,task_index,task_name,algorithm,jobs,wall_seconds,avg_wall_seconds,simulated_seconds,avg_simulated_seconds\n");
    for (int si = 0; si < configuration->stages_number; ++si)
    {
        int sn = configuration->stages[si] - 1;
        struct stage_definition *stage_def = &configuration->stage_definitions[si];
        if (sn >= 0 && sn < 10)
        {
            long combined_jobs = 0;
            double combined_time = 0.0;
            double combined_simulated_time = 0.0;

            for (int task = 0; task < stage_def->input_count; ++task)
            {
                long jobs = nfr_managers_in[sn][task].jobs_processed;
                double time = nfr_managers_in[sn][task].total_processing_time;
                double simulated_time = nfr_managers_in[sn][task].total_simulated_time;
                double avg = (jobs > 0) ? (time / (double)jobs) : 0.0;
                double avg_simulated = (jobs > 0) ? (simulated_time / (double)jobs) : 0.0;
                combined_jobs += jobs;
                combined_time += time;
                combined_simulated_time += simulated_time;
                printf("Stage %d IN task %d (%s:%s): jobs=%ld wall_time=%f s avg_wall=%f s simulated_time=%f s avg_simulated=%f s\n",
                       configuration->stages[si],
                       task,
                       nfr_managers_in[sn][task].task_name,
                       nfr_managers_in[sn][task].task_algorithm,
                       jobs,
                       time,
                       avg,
                       simulated_time,
                       avg_simulated);
                if (manager_csv)
                    fprintf(manager_csv, "%d,%s,input,%d,%s,%s,%ld,%f,%f,%f,%f\n",
                            configuration->stages[si],
                            stage_def->name,
                            task,
                            nfr_managers_in[sn][task].task_name,
                            nfr_managers_in[sn][task].task_algorithm,
                            jobs,
                            time,
                            avg,
                            simulated_time,
                            avg_simulated);
            }

            if (stage_has_application(stage_def))
            {
                long jobs = application_managers[sn].jobs_processed;
                double time = application_managers[sn].total_processing_time;
                double simulated_time = application_managers[sn].total_simulated_time;
                double avg = (jobs > 0) ? (time / (double)jobs) : 0.0;
                double avg_simulated = (jobs > 0) ? (simulated_time / (double)jobs) : 0.0;
                combined_jobs += jobs;
                combined_time += time;
                combined_simulated_time += simulated_time;
                printf("Stage %d APP task 0 (application): jobs=%ld wall_time=%f s avg_wall=%f s simulated_time=%f s avg_simulated=%f s\n",
                       configuration->stages[si],
                       jobs,
                       time,
                       avg,
                       simulated_time,
                       avg_simulated);
                if (manager_csv)
                    fprintf(manager_csv, "%d,%s,application,0,application,,%ld,%f,%f,%f,%f\n",
                            configuration->stages[si],
                            stage_def->name,
                            jobs,
                            time,
                            avg,
                            simulated_time,
                            avg_simulated);
            }

            for (int task = 0; task < stage_def->output_count; ++task)
            {
                long jobs = nfr_managers_out[sn][task].jobs_processed;
                double time = nfr_managers_out[sn][task].total_processing_time;
                double simulated_time = nfr_managers_out[sn][task].total_simulated_time;
                double avg = (jobs > 0) ? (time / (double)jobs) : 0.0;
                double avg_simulated = (jobs > 0) ? (simulated_time / (double)jobs) : 0.0;
                combined_jobs += jobs;
                combined_time += time;
                combined_simulated_time += simulated_time;
                printf("Stage %d OUT task %d (%s:%s): jobs=%ld wall_time=%f s avg_wall=%f s simulated_time=%f s avg_simulated=%f s\n",
                       configuration->stages[si],
                       task,
                       nfr_managers_out[sn][task].task_name,
                       nfr_managers_out[sn][task].task_algorithm,
                       jobs,
                       time,
                       avg,
                       simulated_time,
                       avg_simulated);
                if (manager_csv)
                    fprintf(manager_csv, "%d,%s,output,%d,%s,%s,%ld,%f,%f,%f,%f\n",
                            configuration->stages[si],
                            stage_def->name,
                            task,
                            nfr_managers_out[sn][task].task_name,
                            nfr_managers_out[sn][task].task_algorithm,
                            jobs,
                            time,
                            avg,
                            simulated_time,
                            avg_simulated);
            }

            double combined_avg = (combined_jobs > 0) ? (combined_time / (double)combined_jobs) : 0.0;
            double combined_simulated_avg = (combined_jobs > 0) ? (combined_simulated_time / (double)combined_jobs) : 0.0;

            printf("Stage %d COMBINED: jobs=%ld wall_time=%f s avg_wall=%f s simulated_time=%f s avg_simulated=%f s\n", configuration->stages[si], combined_jobs, combined_time, combined_avg, combined_simulated_time, combined_simulated_avg);
            if (manager_csv)
                fprintf(manager_csv, "%d,%s,combined,-1,combined,,%ld,%f,%f,%f,%f\n",
                        configuration->stages[si],
                        stage_def->name,
                        combined_jobs,
                        combined_time,
                        combined_avg,
                        combined_simulated_time,
                        combined_simulated_avg);
        }
    }
    if (manager_csv)
        fclose(manager_csv);

    printf("\n=== Link Metrics ===\n");
    FILE *link_csv = open_report_csv("link_metrics.csv");
    if (link_csv)
        fprintf(link_csv, "from,to,transfers,bytes,total_seconds\n");
    for (int li = 0; li < configuration->links_number; ++li)
    {
        printf("Link %s->%s transfers=%d bytes=%f total_time=%f s\n", configuration->links[li].from, configuration->links[li].to, configuration->links[li].transfers_count, configuration->links[li].bytes_transferred, configuration->links[li].total_transfer_time);
        if (link_csv)
            fprintf(link_csv, "%s,%s,%d,%f,%f\n",
                    configuration->links[li].from,
                    configuration->links[li].to,
                    configuration->links[li].transfers_count,
                    configuration->links[li].bytes_transferred,
                    configuration->links[li].total_transfer_time);
    }
    if (link_csv)
        fclose(link_csv);
}

/* Wait until all NFR manager queues are drained or timeout (seconds). */
int wait_for_managers_empty(struct config *configuration, int timeout_seconds)
{
    if (!configuration) return -1;
    int waited_ms = 0;
    int timeout_ms = timeout_seconds * 1000;
    int stable_count = 0;
    long last_total = -1;
    while (waited_ms < timeout_ms)
    {
        long total_q = 0;
        for (int si = 0; si < configuration->stages_number; ++si)
        {
            int sn = configuration->stages[si] - 1;
            struct stage_definition *stage_def = &configuration->stage_definitions[si];
            if (sn >= 0 && sn < 10)
            {
                for (int task = 0; task < stage_def->input_count; ++task)
                {
                    pthread_mutex_lock(&nfr_managers_in[sn][task].lock);
                    total_q += nfr_managers_in[sn][task].q_count;
                    pthread_mutex_unlock(&nfr_managers_in[sn][task].lock);
                }
                if (stage_has_application(stage_def) && application_managers[sn].threads)
                {
                    pthread_mutex_lock(&application_managers[sn].lock);
                    total_q += application_managers[sn].q_count;
                    pthread_mutex_unlock(&application_managers[sn].lock);
                }
                for (int task = 0; task < stage_def->output_count; ++task)
                {
                    pthread_mutex_lock(&nfr_managers_out[sn][task].lock);
                    total_q += nfr_managers_out[sn][task].q_count;
                    pthread_mutex_unlock(&nfr_managers_out[sn][task].lock);
                }
            }
        }
        if (total_q == 0)
        {
            if (last_total == 0)
            {
                stable_count++;
            }
            else
            {
                stable_count = 0;
            }
            last_total = 0;
            if (stable_count >= 3)
                return 0; /* drained */
        }
        else
        {
            last_total = total_q;
            stable_count = 0;
        }
        usleep(100 * 1000); /* 100 ms */
        waited_ms += 100;
    }
    return -1; /* timeout */
}

int wait_for_outstanding_zero(int timeout_seconds)
{
    struct timespec ts;
    pthread_mutex_lock(&outstanding_lock);
    if (outstanding_jobs == 0)
    {
        pthread_mutex_unlock(&outstanding_lock);
        return 0;
    }
    /* compute absolute timeout */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_seconds;
    int rc = 0;
    while (outstanding_jobs > 0 && rc == 0)
    {
        rc = pthread_cond_timedwait(&outstanding_cond, &outstanding_lock, &ts);
    }
    pthread_mutex_unlock(&outstanding_lock);
    return (outstanding_jobs == 0) ? 0 : -1;
}


/**
 * @brief Function returns error in the case to occur.
 */
void error(const char *s)
{
    perror(s); //< perror() returns the S string and the error that found in errno.
    exit(EXIT_FAILURE);
}

/**
 * @brief Function that read the JSON config file.
 */
char agent_container_prefix[32] = "output_agent";
/* Optional inline trace first-line provided via config JSON (overrides traces.cfg) */
static char *global_trace_first_line = NULL;
/* Optional structured traces provided via config JSON (preferred). */
static struct traceConfig *global_trace_array = NULL;
static int global_trace_array_count = 0;

struct config *read_config(const char *file_name)
{
    FILE *file;
    struct config *configuration;
    long length;
    char *data;
    cJSON *json, *workers, *traces_number, *traces_fileName, *agent_type, *stages, *stage;

    configuration = malloc(sizeof(struct config));
    if (!configuration)
        error("Memory allocation failed for configuration");
    memset(configuration, 0, sizeof(struct config));
    configuration->stages_number = 0;
    configuration->traces_fileName = malloc((255) * sizeof(char));
    if (!configuration->traces_fileName)
        error("Memory allocation failed for traces file name");
    strcpy(configuration->agent_type, "output");

    file = fopen(file_name, "rb"); //< Read file
    if (!file)
        error("Error opening config.json");

    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc(length + 1);
    if (data)
    {
        fread(data, 1, length, file);
    }
    data[length] = '\0';
    fclose(file);

    json = cJSON_Parse(data);
    if (!json)
    {
        printf("Error before: [%s]\n", cJSON_GetErrorPtr());
        error("Error parsing config.json");
    }

    // Default values
    strcpy(configuration->compression_algo, "");
    strcpy(configuration->hashing_algo, "");
    strcpy(configuration->ida_algo, "");
    strcpy(configuration->service_time_model, "linear");
    strcpy(configuration->container_platform, DEFAULT_CONTAINER_PLATFORM);
    strcpy(configuration->queue_container_image, DEFAULT_QUEUE_DOCKER_IMAGE);
    strcpy(configuration->trace_container_image, DEFAULT_TRACE_DOCKER_IMAGE);
    strcpy(configuration->trace_generator_binary, DEFAULT_TRACE_GENERATOR_BINARY);
    configuration->real_values_dir[0] = '\0';

    workers = cJSON_GetObjectItemCaseSensitive(json, "workers");
    if (cJSON_IsNumber(workers))
    {
        configuration->workers = workers->valueint;
    }

    traces_number = cJSON_GetObjectItemCaseSensitive(json, "traces_number");
    if (cJSON_IsNumber(traces_number))
    {
        configuration->traces_number = traces_number->valueint;
    }

    traces_fileName = cJSON_GetObjectItemCaseSensitive(json, "traces_fileName");
    if (cJSON_IsString(traces_fileName) && (traces_fileName->valuestring != NULL))
    {
        strcpy(configuration->traces_fileName, traces_fileName->valuestring);
    }

    /* New: allow providing the first line of traces.cfg inline in the config JSON
       as a string with the same space-separated fields used in traces.cfg.
       Example: "trace_first_line": "10000 5000.0 3 15 0.6 30 0.5 1" */
    cJSON *trace_first = cJSON_GetObjectItemCaseSensitive(json, "trace_first_line");
    if (cJSON_IsString(trace_first) && trace_first->valuestring) {
        /* store a copy for read_configTrace to consume */
        if (global_trace_first_line) free(global_trace_first_line);
        global_trace_first_line = strdup(trace_first->valuestring);
    }

    /* New: allow providing structured trace configuration in JSON as an array:
       "traces": [ { "MUESTRAS": 100, "inter_arrival": 2.0, "DISTRIBUTION": 3, "mean": 15, "stddev": 0.6, "SIZE": 30000000, "stddevS": 0.5, "Concurrency": 1 }, ... ]
       This is preferred over the single-line string. */
    cJSON *traces_json = cJSON_GetObjectItemCaseSensitive(json, "traces");
    if (cJSON_IsArray(traces_json)) {
        int count = cJSON_GetArraySize(traces_json);
        if (count > 0) {
            if (global_trace_array) { free(global_trace_array); global_trace_array = NULL; global_trace_array_count = 0; }
            int use_count = (configuration->traces_number > 0) ? configuration->traces_number : count;
            global_trace_array = malloc(sizeof(struct traceConfig) * use_count);
            if (global_trace_array) {
                cJSON *t = NULL;
                int idx = 0;
                cJSON_ArrayForEach(t, traces_json) {
                    if (idx >= use_count) break;
                    cJSON *muestras = cJSON_GetObjectItemCaseSensitive(t, "MUESTRAS");
                    if (!muestras) muestras = cJSON_GetObjectItemCaseSensitive(t, "samples");
                    cJSON *inter = cJSON_GetObjectItemCaseSensitive(t, "inter_arrival");
                    cJSON *dist = cJSON_GetObjectItemCaseSensitive(t, "DISTRIBUTION");
                    cJSON *mean = cJSON_GetObjectItemCaseSensitive(t, "mean");
                    cJSON *stddev = cJSON_GetObjectItemCaseSensitive(t, "stddev");
                    cJSON *size = cJSON_GetObjectItemCaseSensitive(t, "SIZE");
                    cJSON *stddevs = cJSON_GetObjectItemCaseSensitive(t, "stddevS");
                    cJSON *conc = cJSON_GetObjectItemCaseSensitive(t, "Concurrency");

                    global_trace_array[idx].MUESTRAS = cJSON_IsNumber(muestras) ? (long long unsigned)muestras->valuedouble : 1;
                    global_trace_array[idx].inter_arrival = cJSON_IsNumber(inter) ? inter->valuedouble : 0.0f;
                    global_trace_array[idx].DISTRIBUTION = cJSON_IsNumber(dist) ? (long long unsigned)dist->valuedouble : 0;
                    global_trace_array[idx].mean = cJSON_IsNumber(mean) ? mean->valuedouble : 0.0f;
                    global_trace_array[idx].stddev = cJSON_IsNumber(stddev) ? stddev->valuedouble : 0.0f;
                    global_trace_array[idx].SIZE = cJSON_IsNumber(size) ? size->valuedouble : 0.0f;
                    global_trace_array[idx].stddevS = cJSON_IsNumber(stddevs) ? stddevs->valuedouble : 0.0f;
                    global_trace_array[idx].Concurrency = cJSON_IsNumber(conc) ? (long long unsigned)conc->valuedouble : 1;
                    idx++;
                }
                /* set actual filled count and discard array if nothing parsed */
                global_trace_array_count = idx;
                if (global_trace_array_count == 0) {
                    free(global_trace_array);
                    global_trace_array = NULL;
                }
            }
        }
    }

    agent_type = cJSON_GetObjectItemCaseSensitive(json, "agent_type");
    if (cJSON_IsString(agent_type) && (agent_type->valuestring != NULL))
    {
        if (strcmp(agent_type->valuestring, "input") == 0 || strcmp(agent_type->valuestring, "output") == 0)
        {
            strcpy(configuration->agent_type, agent_type->valuestring);
        }
        else
        {
            strcpy(configuration->agent_type, "output");
        }
    }

    cJSON *comp_algo = cJSON_GetObjectItemCaseSensitive(json, "compression_algo");
    if (cJSON_IsString(comp_algo) && (comp_algo->valuestring != NULL))
    {
        strcpy(configuration->compression_algo, comp_algo->valuestring);
    }

    cJSON *hash_algo = cJSON_GetObjectItemCaseSensitive(json, "hashing_algo");
    if (cJSON_IsString(hash_algo) && (hash_algo->valuestring != NULL))
    {
        strcpy(configuration->hashing_algo, hash_algo->valuestring);
    }

    cJSON *id_algo = cJSON_GetObjectItemCaseSensitive(json, "ida_algo");
    if (cJSON_IsString(id_algo) && (id_algo->valuestring != NULL))
    {
        strcpy(configuration->ida_algo, id_algo->valuestring);
    }

    cJSON *service_time_model = cJSON_GetObjectItemCaseSensitive(json, "service_time_model");
    if (!cJSON_IsString(service_time_model))
        service_time_model = cJSON_GetObjectItemCaseSensitive(json, "interpolation_model");
    if (!cJSON_IsString(service_time_model))
        service_time_model = cJSON_GetObjectItemCaseSensitive(json, "modeling_model");
    if (cJSON_IsString(service_time_model) && service_time_model->valuestring != NULL)
    {
        strncpy(configuration->service_time_model, service_time_model->valuestring, sizeof(configuration->service_time_model) - 1);
        configuration->service_time_model[sizeof(configuration->service_time_model) - 1] = '\0';
    }

    static const char *container_platform_keys[] = {
        "container_platform",
        "container_runtime",
        "runtime"
    };
    cJSON *container_platform = first_string_item(json, container_platform_keys, sizeof(container_platform_keys) / sizeof(container_platform_keys[0]));
    if (container_platform)
    {
        strncpy(configuration->container_platform, container_platform->valuestring, sizeof(configuration->container_platform) - 1);
        configuration->container_platform[sizeof(configuration->container_platform) - 1] = '\0';
    }

    static const char *queue_image_keys[] = {
        "queue_container_image",
        "queue_image",
        "single_queue_image",
        "queue_sif",
        "sif_path",
        "apptainer_sif"
    };
    cJSON *queue_image = first_string_item(json, queue_image_keys, sizeof(queue_image_keys) / sizeof(queue_image_keys[0]));
    if (queue_image)
    {
        strncpy(configuration->queue_container_image, queue_image->valuestring, sizeof(configuration->queue_container_image) - 1);
        configuration->queue_container_image[sizeof(configuration->queue_container_image) - 1] = '\0';
    }

    static const char *trace_image_keys[] = {
        "trace_container_image",
        "trace_generator_image",
        "trace_generator_sif"
    };
    cJSON *trace_image = first_string_item(json, trace_image_keys, sizeof(trace_image_keys) / sizeof(trace_image_keys[0]));
    if (trace_image)
    {
        strncpy(configuration->trace_container_image, trace_image->valuestring, sizeof(configuration->trace_container_image) - 1);
        configuration->trace_container_image[sizeof(configuration->trace_container_image) - 1] = '\0';
    }

    static const char *trace_binary_keys[] = {
        "trace_generator_binary",
        "trace_generator_executable"
    };
    cJSON *trace_binary = first_string_item(json, trace_binary_keys, sizeof(trace_binary_keys) / sizeof(trace_binary_keys[0]));
    if (trace_binary)
    {
        strncpy(configuration->trace_generator_binary, trace_binary->valuestring, sizeof(configuration->trace_generator_binary) - 1);
        configuration->trace_generator_binary[sizeof(configuration->trace_generator_binary) - 1] = '\0';
    }

    cJSON *id_k = cJSON_GetObjectItemCaseSensitive(json, "ida_k");
    if (cJSON_IsNumber(id_k))
    {
        configuration->ida_k = id_k->valueint;
    }
    else
    {
        configuration->ida_k = 4; // Default
    }

    cJSON *id_m = cJSON_GetObjectItemCaseSensitive(json, "ida_m");
    if (cJSON_IsNumber(id_m))
    {
        configuration->ida_m = id_m->valueint;
    }
    else
    {
        configuration->ida_m = 2; // Default
    }

    configuration->aes_key_bits = 256; // Default AES key size.
    cJSON *aes_key_bits = cJSON_GetObjectItemCaseSensitive(json, "aes_key_bits");
    if (!cJSON_IsNumber(aes_key_bits))
        aes_key_bits = cJSON_GetObjectItemCaseSensitive(json, "key_bits");
    if (cJSON_IsNumber(aes_key_bits))
    {
        configuration->aes_key_bits = aes_key_bits->valueint;
    }

    cJSON *real_values_dir = cJSON_GetObjectItemCaseSensitive(json, "real_values_dir");
    if (cJSON_IsString(real_values_dir) && real_values_dir->valuestring != NULL)
    {
        strncpy(configuration->real_values_dir, real_values_dir->valuestring, sizeof(configuration->real_values_dir) - 1);
        configuration->real_values_dir[sizeof(configuration->real_values_dir) - 1] = '\0';
    }

    cJSON *bfs = cJSON_GetObjectItemCaseSensitive(json, "b_fs");
    if (cJSON_IsNumber(bfs)) {
        /* configuration file provides b_fs in MB/s — convert to bytes/sec */
        configuration->b_fs = bfs->valuedouble * 1048576.0;
    } else {
        configuration->b_fs = 100.0 * 1048576.0; // Default 100 MB/s
    }
    configuration->b_fs_read = configuration->b_fs;
    configuration->b_fs_write = configuration->b_fs;

    cJSON *bfs_read = cJSON_GetObjectItemCaseSensitive(json, "b_fs_read");
    if (cJSON_IsNumber(bfs_read))
        configuration->b_fs_read = bfs_read->valuedouble * 1048576.0;
    cJSON *bfs_write = cJSON_GetObjectItemCaseSensitive(json, "b_fs_write");
    if (cJSON_IsNumber(bfs_write))
        configuration->b_fs_write = bfs_write->valuedouble * 1048576.0;

    cJSON *bfs_read_bytes = cJSON_GetObjectItemCaseSensitive(json, "b_fs_read_bytes");
    if (!cJSON_IsNumber(bfs_read_bytes))
        bfs_read_bytes = cJSON_GetObjectItemCaseSensitive(json, "b_fs_read_bytes_per_sec");
    if (cJSON_IsNumber(bfs_read_bytes))
        configuration->b_fs_read = bfs_read_bytes->valuedouble;

    cJSON *bfs_write_bytes = cJSON_GetObjectItemCaseSensitive(json, "b_fs_write_bytes");
    if (!cJSON_IsNumber(bfs_write_bytes))
        bfs_write_bytes = cJSON_GetObjectItemCaseSensitive(json, "b_fs_write_bytes_per_sec");
    if (cJSON_IsNumber(bfs_write_bytes))
        configuration->b_fs_write = bfs_write_bytes->valuedouble;

    static const char *application_time_keys[] = {
        "application_mean_service_time",
        "application_mean_execution_time",
        "application_avg_execution_time",
        "application_execution_time"
    };
    cJSON *app_time = first_number_item(json, application_time_keys, sizeof(application_time_keys) / sizeof(application_time_keys[0]));
    configuration->application_mean_service_time = cJSON_IsNumber(app_time) ? app_time->valuedouble : 0.0;

    cJSON *concurrency_penalty_json = cJSON_GetObjectItemCaseSensitive(json, "concurrency_penalty");
    configuration->concurrency_penalty = cJSON_IsNumber(concurrency_penalty_json) ? concurrency_penalty_json->valuedouble : 0.0;

    stages = cJSON_GetObjectItemCaseSensitive(json, "stages");
    if (cJSON_IsArray(stages))
    {
        cJSON_ArrayForEach(stage, stages)
        {
            if (configuration->stages_number >= MAX_STAGES)
                break;

            if (cJSON_IsString(stage) && stage->valuestring)
            {
                int parsed_stage = parse_stage_identifier(stage->valuestring);
                if (parsed_stage > 0 && parsed_stage <= 10)
                {
                    int si = configuration->stages_number;
                    configuration->stages[configuration->stages_number++] = parsed_stage;
                    init_stage_definition(configuration, si, parsed_stage, stage->valuestring);
                    add_default_output_requirements(configuration, &configuration->stage_definitions[si]);
                }
            }
            else if (cJSON_IsObject(stage))
            {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(stage, "name");
                cJSON *id = cJSON_GetObjectItemCaseSensitive(stage, "id");
                const char *stage_name = NULL;
                int parsed_stage = 0;

                if (cJSON_IsString(name) && name->valuestring)
                {
                    stage_name = name->valuestring;
                    parsed_stage = parse_stage_identifier(stage_name);
                }
                if (parsed_stage <= 0 && cJSON_IsString(id) && id->valuestring)
                {
                    stage_name = id->valuestring;
                    parsed_stage = parse_stage_identifier(id->valuestring);
                }
                if (parsed_stage <= 0 && cJSON_IsNumber(id))
                {
                    parsed_stage = id->valueint;
                }

                if (parsed_stage > 0 && parsed_stage <= 10)
                {
                    int si = configuration->stages_number;
                    configuration->stages[configuration->stages_number++] = parsed_stage;
                    init_stage_definition(configuration, si, parsed_stage, stage_name);
                    parse_stage_arrival_mode(stage, &configuration->stage_definitions[si]);
                    parse_stage_mean_interarrival(stage, &configuration->stage_definitions[si]);
                    parse_stage_filesystem_bandwidth(stage, &configuration->stage_definitions[si]);
                    parse_stage_application_time(stage, &configuration->stage_definitions[si]);
                    parse_stage_application_size_factor(stage, &configuration->stage_definitions[si]);

                    cJSON *output_reqs = cJSON_GetObjectItemCaseSensitive(stage, "output_requirements");
                    if (!cJSON_IsArray(output_reqs))
                        output_reqs = cJSON_GetObjectItemCaseSensitive(stage, "output_nfrs");
                    if (!cJSON_IsArray(output_reqs))
                        output_reqs = cJSON_GetObjectItemCaseSensitive(stage, "requirements");

                    if (!parse_requirements_array(configuration, output_reqs, &configuration->stage_definitions[si], 0))
                        add_default_output_requirements(configuration, &configuration->stage_definitions[si]);

                    cJSON *input_reqs = cJSON_GetObjectItemCaseSensitive(stage, "input_requirements");
                    if (!cJSON_IsArray(input_reqs))
                        input_reqs = cJSON_GetObjectItemCaseSensitive(stage, "input_nfrs");
                    parse_requirements_array(configuration, input_reqs, &configuration->stage_definitions[si], 1);
                }
            }
        }
    }

    finalize_stage_pipelines(configuration);

        /* Optional distributed machines specification
           Format: "machines": [ {"name":"m0","stages":["stage1"]}, ... ]
           Older operation names are still accepted as stage identifiers.
        */
        configuration->machines_number = 0;
        cJSON *machines = cJSON_GetObjectItemCaseSensitive(json, "machines");
        if (cJSON_IsArray(machines)) {
            cJSON *m;
            int m_idx = 0;
            cJSON_ArrayForEach(m, machines) {
                if (m_idx >= MAX_MACHINES) break;
                cJSON *mname = cJSON_GetObjectItemCaseSensitive(m, "name");
                cJSON *mstages = cJSON_GetObjectItemCaseSensitive(m, "stages");
                cJSON *mprofile = cJSON_GetObjectItemCaseSensitive(m, "hardware_profile");
                cJSON *mprofile_alt = cJSON_GetObjectItemCaseSensitive(m, "profile");
                cJSON *mvalues = cJSON_GetObjectItemCaseSensitive(m, "real_values_dir");
                if (cJSON_IsString(mname) && mname->valuestring) {
                    strncpy(configuration->machines[m_idx].name, mname->valuestring, sizeof(configuration->machines[m_idx].name)-1);
                    configuration->machines[m_idx].name[sizeof(configuration->machines[m_idx].name)-1] = '\0';
                } else {
                    snprintf(configuration->machines[m_idx].name, sizeof(configuration->machines[m_idx].name), "machine%d", m_idx);
                }
                configuration->machines[m_idx].hardware_profile[0] = '\0';
                configuration->machines[m_idx].real_values_dir[0] = '\0';
                configuration->machines[m_idx].service_profile_index = 0;
                if (cJSON_IsString(mprofile) && mprofile->valuestring) {
                    strncpy(configuration->machines[m_idx].hardware_profile, mprofile->valuestring, sizeof(configuration->machines[m_idx].hardware_profile) - 1);
                    configuration->machines[m_idx].hardware_profile[sizeof(configuration->machines[m_idx].hardware_profile) - 1] = '\0';
                } else if (cJSON_IsString(mprofile_alt) && mprofile_alt->valuestring) {
                    strncpy(configuration->machines[m_idx].hardware_profile, mprofile_alt->valuestring, sizeof(configuration->machines[m_idx].hardware_profile) - 1);
                    configuration->machines[m_idx].hardware_profile[sizeof(configuration->machines[m_idx].hardware_profile) - 1] = '\0';
                }
                if (cJSON_IsString(mvalues) && mvalues->valuestring) {
                    strncpy(configuration->machines[m_idx].real_values_dir, mvalues->valuestring, sizeof(configuration->machines[m_idx].real_values_dir) - 1);
                    configuration->machines[m_idx].real_values_dir[sizeof(configuration->machines[m_idx].real_values_dir) - 1] = '\0';
                }
                configuration->machines[m_idx].stages_number = 0;
                if (cJSON_IsArray(mstages)) {
                    cJSON *ms;
                    cJSON_ArrayForEach(ms, mstages) {
                        if (cJSON_IsString(ms) && configuration->machines[m_idx].stages_number < 10) {
                            int parsed_stage = parse_stage_identifier(ms->valuestring);
                            if (parsed_stage > 0 && parsed_stage <= 10)
                                configuration->machines[m_idx].stages[configuration->machines[m_idx].stages_number++] = parsed_stage;
                        }
                    }
                }
                m_idx++;
            }
            configuration->machines_number = m_idx;
        }

        /* Optional network links between machines: {"from":"m0","to":"m1","b_net":100} (MB/s) */
        configuration->links_number = 0;
        cJSON *links = cJSON_GetObjectItemCaseSensitive(json, "links");
        if (cJSON_IsArray(links)) {
            cJSON *l;
            int l_idx = 0;
            cJSON_ArrayForEach(l, links) {
                if (l_idx >= MAX_LINKS) break;
                cJSON *from = cJSON_GetObjectItemCaseSensitive(l, "from");
                cJSON *to = cJSON_GetObjectItemCaseSensitive(l, "to");
                cJSON *bnet = cJSON_GetObjectItemCaseSensitive(l, "b_net");
                cJSON *lat = cJSON_GetObjectItemCaseSensitive(l, "latency_ms");
                if (cJSON_IsString(from) && from->valuestring) {
                    strncpy(configuration->links[l_idx].from, from->valuestring, sizeof(configuration->links[l_idx].from)-1);
                    configuration->links[l_idx].from[sizeof(configuration->links[l_idx].from)-1] = '\0';
                } else configuration->links[l_idx].from[0] = '\0';
                if (cJSON_IsString(to) && to->valuestring) {
                    strncpy(configuration->links[l_idx].to, to->valuestring, sizeof(configuration->links[l_idx].to)-1);
                    configuration->links[l_idx].to[sizeof(configuration->links[l_idx].to)-1] = '\0';
                } else configuration->links[l_idx].to[0] = '\0';
                configuration->links[l_idx].b_net = 0.0;
                if (cJSON_IsNumber(bnet)) configuration->links[l_idx].b_net = bnet->valuedouble * 1048576.0; /* MB/s -> bytes/sec */
                configuration->links[l_idx].latency_ms = 0.0;
                if (cJSON_IsNumber(lat)) configuration->links[l_idx].latency_ms = lat->valuedouble;
                l_idx++;
            }
            configuration->links_number = l_idx;
        }

    cJSON_Delete(json);
    free(data);
    return configuration;
}

struct traceConfig *read_configTrace(int numberTrace, char *fileName)
{
    FILE *file;
    char *token, *delimitador, line[500], key[200], value[400];
    int linenum, cont, i;
    struct traceConfig *traceData;

    traceData = malloc(sizeof(struct traceConfig) * numberTrace);
    token = NULL;
    delimitador = " ";

    if (!traceData)
        error("Memory allocation failed for traceData");

    linenum = 0;
    /* If an inline first-line was provided in the JSON config, use it instead of reading a traces file. */
    if (global_trace_first_line != NULL && strlen(global_trace_first_line) > 0) {
        /* parse tokens from the provided string and replicate into all trace entries */
        char *copy = strdup(global_trace_first_line);
        char *tok = NULL;
        char *saveptr = NULL;
        int fields[8];
        double ffields[8];
        int parsed = 0;

        tok = strtok_r(copy, " ", &saveptr);
        while (tok != NULL && parsed < 8) {
            /* store as both int and double to ease assignment */
            ffields[parsed] = atof(tok);
            fields[parsed] = atoi(tok);
            parsed++;
            tok = strtok_r(NULL, " ", &saveptr);
        }

        for (cont = 0; cont < numberTrace; ++cont) {
            traceData[cont].MUESTRAS = (parsed >= 1) ? fields[0] : 0;
            traceData[cont].inter_arrival = (parsed >= 2) ? ffields[1] : 0.0;
            traceData[cont].DISTRIBUTION = (parsed >= 3) ? fields[2] : 0;
            traceData[cont].mean = (parsed >= 4) ? ffields[3] : 0.0;
            traceData[cont].stddev = (parsed >= 5) ? ffields[4] : 0.0;
            traceData[cont].SIZE = (parsed >= 6) ? ffields[5] : 0.0;
            traceData[cont].stddevS = (parsed >= 7) ? ffields[6] : 0.0;
            traceData[cont].Concurrency = (parsed >= 8) ? fields[7] : 0;
            traceData[cont].MUESTRAS = traceData[cont].MUESTRAS > 0 ? traceData[cont].MUESTRAS : 1;
        }
        free(copy);
    } else if (global_trace_array != NULL && global_trace_array_count > 0) {
        /* Use structured traces provided via JSON */
        for (cont = 0; cont < numberTrace; ++cont) {
            int src = cont < global_trace_array_count ? cont : 0;
            traceData[cont].MUESTRAS = global_trace_array[src].MUESTRAS;
            traceData[cont].inter_arrival = global_trace_array[src].inter_arrival;
            traceData[cont].DISTRIBUTION = global_trace_array[src].DISTRIBUTION;
            traceData[cont].mean = global_trace_array[src].mean;
            traceData[cont].stddev = global_trace_array[src].stddev;
            traceData[cont].SIZE = global_trace_array[src].SIZE;
            traceData[cont].stddevS = global_trace_array[src].stddevS;
            traceData[cont].Concurrency = global_trace_array[src].Concurrency;
            traceData[cont].MUESTRAS = traceData[cont].MUESTRAS > 0 ? traceData[cont].MUESTRAS : 1;
        }
    } else {
        file = fopen(fileName, "r"); //< Read file
        if (!file)
            error("Error opening trace configuration file");
        cont = 0;
        i = 1;

        while (fgets(line, 256, file) != NULL)
        {
            linenum++;
            if (line[0] == '#')
                continue;

            delimitador = " ";
            token = strtok(line, delimitador);

            if (token != NULL)
            {
                while (token != NULL)
                {
                    switch (i)
                    {

                    case 1:
                        strcpy(value, token);
                        traceData[cont].MUESTRAS = atoi(value);
                        break;
                    case 2:
                        strcpy(value, token);
                        traceData[cont].inter_arrival = atof(value);
                        break;
                    case 3:
                        strcpy(value, token);
                        traceData[cont].DISTRIBUTION = atoi(value);
                        break;
                    case 4:
                        strcpy(value, token);
                        traceData[cont].mean = atof(value);
                        break;
                    case 5:
                        strcpy(value, token);
                        traceData[cont].stddev = atof(value);
                        break;
                    case 6:
                        strcpy(value, token);
                        traceData[cont].SIZE = atof(value);
                        break;
                    case 7:
                        strcpy(value, token);
                        traceData[cont].stddevS = atof(value);
                        break;
                    case 8:
                        strcpy(value, token);
                        traceData[cont].Concurrency = atoi(value);
                        break;
                    }

                    token = strtok(NULL, delimitador);

                    i++;

                    if (i > 8)
                    {
                        i = 1;
                    }
                }
            }
            cont++;
            if (cont >= numberTrace)
                break;
        }
        fclose(file);
    }
    return traceData;
}

static char *build_queue_estimator_command(const char *container_prefix,
                                           int worker_id,
                                           double mean_interarrival,
                                           double mean_service,
                                           int samples)
{
    int command_size;
    char *command;

    if (active_runtime_is_apptainer())
    {
        char *image = shell_quote(active_queue_container_image);
        if (!image)
            return NULL;

        command_size = snprintf(NULL, 0,
                                "%s run %s %f %f %d",
                                active_runtime_command(),
                                image,
                                mean_interarrival,
                                mean_service,
                                samples) + 1;
        command = malloc(command_size);
        if (command)
        {
            snprintf(command, command_size,
                     "%s run %s %f %f %d",
                     active_runtime_command(),
                     image,
                     mean_interarrival,
                     mean_service,
                     samples);
        }
        free(image);
        return command;
    }

    command_size = snprintf(NULL, 0,
                            "docker exec %s%d ./single %f %f %d",
                            container_prefix,
                            worker_id,
                            mean_interarrival,
                            mean_service,
                            samples) + 1;
    command = malloc(command_size);
    if (!command)
        return NULL;

    snprintf(command, command_size,
             "docker exec %s%d ./single %f %f %d",
             container_prefix,
             worker_id,
             mean_interarrival,
             mean_service,
             samples);
    return command;
}

static char *build_queue_estimator_redirect_command(const char *container_prefix,
                                                    int worker_id,
                                                    double mean_interarrival,
                                                    double mean_service,
                                                    int samples,
                                                    const char *result_path)
{
    char *base = build_queue_estimator_command(container_prefix, worker_id, mean_interarrival, mean_service, samples);
    char *quoted_result_path;
    int command_size;
    char *command;

    if (!base)
        return NULL;

    quoted_result_path = shell_quote(result_path);
    if (!quoted_result_path)
    {
        free(base);
        return NULL;
    }

    command_size = snprintf(NULL, 0, "%s >> %s", base, quoted_result_path) + 1;
    command = malloc(command_size);
    if (command)
        snprintf(command, command_size, "%s >> %s", base, quoted_result_path);

    free(base);
    free(quoted_result_path);
    return command;
}

void makeTraceGenerator()
{
    char *command, *pwd;
    int size;

    if (active_runtime_is_apptainer())
    {
        if (active_trace_container_image[0] != '\0')
            printf("Using %s trace generator image %s\n", active_runtime_command(), active_trace_container_image);
        else
            printf("Using native trace generator %s\n", active_trace_generator_binary);
        return;
    }

    pwd = getenv("PWD");
    if (!pwd)
        return;

    char *quoted_image = shell_quote(active_trace_container_image);
    if (!quoted_image)
        return;

    size = snprintf(NULL, 0,
                    "docker ps -a --format '{{.Names}}' | grep -Eq '^trace_generator$' || "
                    "docker run -i -d --name trace_generator -v '%s/traces/':'%s' %s",
                    pwd, pwd, quoted_image) + 1;
    command = malloc(size * sizeof(char));
    if (!command)
    {
        free(quoted_image);
        return;
    }

    sprintf(
        command,
        "docker ps -a --format '{{.Names}}' | grep -Eq '^trace_generator$' || "
        "docker run -i -d --name trace_generator -v '%s/traces/':'%s' %s",
        pwd, pwd, quoted_image);

    execute_command(command);
    free(quoted_image);
    free(command);
}

void makeAgents(int workers, const char *agent_type)
{
    char *command, *pwd;
    int i;
    const char *container_prefix = "output_agent";

    if (active_runtime_is_apptainer())
    {
        printf("Apptainer selected: not starting long-running %s queue agents; %s runs %s per queue estimate\n",
               agent_type ? agent_type : "worker",
               active_runtime_command(),
               active_queue_container_image);
        return;
    }

    if (strcmp(agent_type, "input") == 0)
        container_prefix = "input_agent";
    else if (strcmp(agent_type, "application") == 0)
        container_prefix = "application_agent";

    pwd = getenv("PWD");
    if (!pwd)
        return;

    for (i = 0; i < workers; ++i)
    {
        char *quoted_image = shell_quote(active_queue_container_image);
        if (!quoted_image)
            return;

        int size = snprintf(NULL, 0,
                            "docker ps -a --format '{{.Names}}' | grep -Eq '^%s%d$' || "
                            "docker run -i -d --name %s%d -v '%s/traces/':'%s' %s",
                            container_prefix, i,
                            container_prefix, i,
                            pwd, pwd,
                            quoted_image) + 1;
        command = malloc(size * sizeof(char));
        if (!command)
        {
            free(quoted_image);
            return;
        }

        sprintf(
            command,
            "docker ps -a --format '{{.Names}}' | grep -Eq '^%s%d$' || "
            "docker run -i -d --name %s%d -v '%s/traces/':'%s' %s",
            container_prefix, i,
            container_prefix, i,
            pwd, pwd,
            quoted_image);

        printf("Starting %s queue agent %d with command: %s\n", agent_type ? agent_type : "worker", i, command);

        execute_command(command);
        free(quoted_image);
        free(command);
    }
}

void makeContainers(struct config *configuration)
{
    int needs_application_agents = 0;
    if (configuration)
    {
        for (int si = 0; si < configuration->stages_number; ++si)
        {
            if (stage_has_application(&configuration->stage_definitions[si]))
            {
                needs_application_agents = 1;
                break;
            }
        }
    }

    if (!has_inline_traces())
        makeTraceGenerator();
    makeAgents(configuration->workers, "input");
    if (needs_application_agents)
        makeAgents(configuration->workers, "application");
    makeAgents(configuration->workers, "output");
}

void traceGenerator(struct traceConfig *traceData, int numberTraces)
{
    char *fileName, *command, *pwd;
    const char *baseName = "trace%lld.txt";
    int i;

    pwd = getenv("PWD");
    if (!pwd)
        return;

    for (i = 0; i < numberTraces; ++i)
    {
        char result_path[1024];
        char *quoted_result_path;
        command = NULL;

        fileName = malloc(sizeof(char) + strlen(baseName) + 100);
        sprintf(fileName,
                baseName, i);
        snprintf(result_path, sizeof(result_path), "%s/traces/%s", pwd, fileName);
        quoted_result_path = shell_quote(result_path);
        if (!quoted_result_path)
        {
            free(fileName);
            continue;
        }

        if (active_runtime_is_apptainer())
        {
            char *runner = active_trace_container_image[0] != '\0'
                               ? shell_quote(active_trace_container_image)
                               : shell_quote(active_trace_generator_binary);
            const char *command_template = active_trace_container_image[0] != '\0'
                                               ? "%s run %s %lld %f %lld %f %f %f %f %lld > %s"
                                               : "%s %lld %f %lld %f %f %f %f %lld > %s";
            if (!runner)
            {
                free(quoted_result_path);
                free(fileName);
                continue;
            }

            if (active_trace_container_image[0] != '\0')
            {
                int command_size = snprintf(NULL, 0,
                                            command_template,
                                            active_runtime_command(),
                                            runner,
                                            traceData[i].MUESTRAS,
                                            traceData[i].inter_arrival,
                                            traceData[i].DISTRIBUTION,
                                            traceData[i].mean,
                                            traceData[i].stddev,
                                            traceData[i].SIZE,
                                            traceData[i].stddevS,
                                            traceData[i].Concurrency,
                                            quoted_result_path) + 1;
                command = malloc(command_size);
                if (command)
                    snprintf(command,
                             command_size,
                             command_template,
                             active_runtime_command(),
                             runner,
                             traceData[i].MUESTRAS,
                             traceData[i].inter_arrival,
                             traceData[i].DISTRIBUTION,
                             traceData[i].mean,
                             traceData[i].stddev,
                             traceData[i].SIZE,
                             traceData[i].stddevS,
                             traceData[i].Concurrency,
                             quoted_result_path);
            }
            else
            {
                int command_size = snprintf(NULL, 0,
                                            command_template,
                                            runner,
                                            traceData[i].MUESTRAS,
                                            traceData[i].inter_arrival,
                                            traceData[i].DISTRIBUTION,
                                            traceData[i].mean,
                                            traceData[i].stddev,
                                            traceData[i].SIZE,
                                            traceData[i].stddevS,
                                            traceData[i].Concurrency,
                                            quoted_result_path) + 1;
                command = malloc(command_size);
                if (command)
                    snprintf(command,
                             command_size,
                             command_template,
                             runner,
                             traceData[i].MUESTRAS,
                             traceData[i].inter_arrival,
                             traceData[i].DISTRIBUTION,
                             traceData[i].mean,
                             traceData[i].stddev,
                             traceData[i].SIZE,
                             traceData[i].stddevS,
                             traceData[i].Concurrency,
                             quoted_result_path);
            }
            free(runner);
        }
        else
        {
            int command_size = snprintf(NULL, 0,
                                        "docker exec trace_generator ./main %lld %f %lld %f %f %f %f %lld > %s",
                                        traceData[i].MUESTRAS,
                                        traceData[i].inter_arrival,
                                        traceData[i].DISTRIBUTION,
                                        traceData[i].mean,
                                        traceData[i].stddev,
                                        traceData[i].SIZE,
                                        traceData[i].stddevS,
                                        traceData[i].Concurrency,
                                        quoted_result_path) + 1;
            command = malloc(command_size);
            if (command)
                snprintf(command,
                         command_size,
                         "docker exec trace_generator ./main %lld %f %lld %f %f %f %f %lld > %s",
                         traceData[i].MUESTRAS,
                         traceData[i].inter_arrival,
                         traceData[i].DISTRIBUTION,
                         traceData[i].mean,
                         traceData[i].stddev,
                         traceData[i].SIZE,
                         traceData[i].stddevS,
                         traceData[i].Concurrency,
                         quoted_result_path);
        }

        if (command)
        {
            execute_command(command);
            free(command);
        }

        free(quoted_result_path);
        free(fileName);
    }
}

int has_inline_traces(void)
{
    return ((global_trace_array != NULL && global_trace_array_count > 0) || (global_trace_first_line != NULL));
}

void execute_command(char *command)
{
    FILE *fp;
    fp = popen(command, "r");
    if (fp == NULL)
    {
        printf("Failed to run command to execute trace_generator container\n");
        exit(1);
    }
    pclose(fp);
}

/**
 * @brief Function that allows to obtain size of the file.
 */
long fileSize(char *fname)
{
    long ftam = -1;
    struct stat fdata;
    int error;

    ftam = -1;
    error = stat(fname, &fdata);
    if (error >= 0)
        ftam = fdata.st_size;
    else
        printf("FileName: %s ERRNO: %d - %s\n", fname, errno, strerror(errno));

    return ftam;
}

/**
 * @brief Function to load balanced.
 */
struct worker *assignation(struct config *configuration, struct traceConfig *traceData)
{
    struct worker *arrayWorkers;
    struct traces *traces;
    const char *baseName;
    char *fileName, *pwd;
    int i, position, position1, position2;
    int *contar;
    FILE *fp;
    char line[256];
    long long unsigned interarrival, size;

    global_config = configuration;

    arrayWorkers = (struct worker *)malloc(configuration->workers * sizeof(struct worker));
    contar = malloc(configuration->workers * sizeof(int));

    for (int i = 0; i < configuration->workers; ++i)
    {
        contar[i] = 0;
        arrayWorkers[i].id = i;
        arrayWorkers[i].sizeWorker = 0;
        arrayWorkers[i].sizeStorage = 0;
        arrayWorkers[i].input_workload_size = 0;
        arrayWorkers[i].output_workload_size = 0;
        arrayWorkers[i].interarrive = 0;
        strncpy(arrayWorkers[i].agent_type, "input", sizeof(arrayWorkers[i].agent_type) - 1);
        arrayWorkers[i].agent_type[sizeof(arrayWorkers[i].agent_type) - 1] = '\0';
        arrayWorkers[i].pipeline_is_input = 1;
        arrayWorkers[i].task_id = 0;
        arrayWorkers[i].task_type = NFR_NONE;
        arrayWorkers[i].task_name[0] = '\0';
        arrayWorkers[i].task_algorithm[0] = '\0';
        arrayWorkers[i].b_fs = configuration->b_fs;
        arrayWorkers[i].b_fs_read = configuration->b_fs_read;
        arrayWorkers[i].b_fs_write = configuration->b_fs_write;
        for (int si = 0; si < MAX_STAGES; ++si)
        {
            arrayWorkers[i].stage_input_time[si] = 0.0;
            arrayWorkers[i].stage_output_time[si] = 0.0;
            arrayWorkers[i].stage_application_time[si] = 0.0;
            arrayWorkers[i].stage_transfer_time[si] = 0.0;
            arrayWorkers[i].stage_input_size[si] = 0;
            arrayWorkers[i].stage_output_size[si] = 0;
            for (int nf = 0; nf < NFR_COUNT; ++nf)
            {
                arrayWorkers[i].stage_nfr_input_time[si][nf] = 0.0;
                arrayWorkers[i].stage_nfr_output_time[si][nf] = 0.0;
            }
            for (int task = 0; task < MAX_PIPELINE_TASKS; ++task)
            {
                arrayWorkers[i].stage_input_requirement_time[si][task] = 0.0;
                arrayWorkers[i].stage_output_requirement_time[si][task] = 0.0;
                arrayWorkers[i].stage_input_requirement_input_size[si][task] = 0;
                arrayWorkers[i].stage_input_requirement_output_size[si][task] = 0;
                arrayWorkers[i].stage_output_requirement_input_size[si][task] = 0;
                arrayWorkers[i].stage_output_requirement_output_size[si][task] = 0;
            }
        }
        arrayWorkers[i].trace = malloc(sizeof(struct traces) * traceData[0].MUESTRAS);
        /* initialize trace entries to safe defaults to avoid garbage values */
        if (arrayWorkers[i].trace) {
            for (int ti = 0; ti < traceData[0].MUESTRAS; ++ti) {
                arrayWorkers[i].trace[ti].traceName = NULL;
                arrayWorkers[i].trace[ti].size = 0;
                arrayWorkers[i].trace[ti].size_restore_top = 0;
                for (int rs = 0; rs < MAX_SIZE_RESTORE_STACK; ++rs)
                    arrayWorkers[i].trace[ti].size_restore_stack[rs] = 0.0;
                arrayWorkers[i].trace[ti].mean_interarrival = 0.0f;
                arrayWorkers[i].trace[ti].service_time_c = 0.0f;
                arrayWorkers[i].trace[ti].service_time_h = 0.0f;
                arrayWorkers[i].trace[ti].service_time_idx = 0.0f;
                arrayWorkers[i].trace[ti].service_time_ida = 0.0f;
                arrayWorkers[i].trace[ti].service_time_io = 0.0f;
                arrayWorkers[i].trace[ti].service_time_app = 0.0f;
                arrayWorkers[i].trace[ti].MUESTRAS = 0;
            }
        }
        arrayWorkers[i].service_time = 0.0f;

        /* All object batches enter the first configured stage; the manager chain moves them
           through subsequent stages and updates the active machine. */
        if (configuration->stages_number > 0)
            arrayWorkers[i].stage_owner = configuration->stages[0];
        else
            arrayWorkers[i].stage_owner = 1;

        arrayWorkers[i].machine_id = stage_machine_id(arrayWorkers[i].stage_owner);
        arrayWorkers[i].service_profile_index = machine_service_profile_index(arrayWorkers[i].machine_id);
    }

    /* Initialize NFR managers for every task in each configured stage.
       Number of NFR worker threads per manager is proportional to workers/stages. */
    if (!nfr_initialized)
    {
        int per_stage_threads = configuration->workers / (configuration->stages_number > 0 ? configuration->stages_number : 1);
        if (per_stage_threads < 1) per_stage_threads = 1;
        for (int si = 0; si < configuration->stages_number; ++si)
        {
            int stageNum = configuration->stages[si];
            int idx = stageNum - 1;
            struct stage_definition *stage_def = &configuration->stage_definitions[si];
            if (idx < 0 || idx >= 10)
                continue;
            for (int task = 0; task < stage_def->input_count; ++task)
            {
                struct nfr_requirement *req = &stage_def->input_requirements[task];
                nfr_manager_init(&nfr_managers_in[idx][task], stageNum, task, req->type, req->task_name, req->algorithm, per_stage_threads, 1024, 1, 0);
            }
            if (stage_has_application(stage_def))
                nfr_manager_init(&application_managers[idx], stageNum, 0, NFR_NONE, "application", "", per_stage_threads, 1024, 0, 1);
            for (int task = 0; task < stage_def->output_count; ++task)
            {
                struct nfr_requirement *req = &stage_def->output_requirements[task];
                nfr_manager_init(&nfr_managers_out[idx][task], stageNum, task, req->type, req->task_name, req->algorithm, per_stage_threads, 1024, 0, 0);
            }
        }
        nfr_initialized = 1;
    }

    srand(time(NULL));

    /* Generate traces in-memory from traceData (no external trace files). */
    traces = malloc(sizeof(struct traces) * traceData[0].MUESTRAS);
    if (!traces) {
        printf("Error allocating traces array\n");
        exit(1);
    }

    /* helper: normal RNG via Box-Muller */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
    auto_generate:
    i = 0;
    while (i < traceData[0].MUESTRAS)
    {
        double base_size = traceData[0].SIZE;
        double stds = traceData[0].stddevS;
        double size_val = base_size;
        if (stds > 0.0) {
            double u1 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
            double u2 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
            double z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
            size_val = base_size + z0 * stds;
            if (size_val < 0.0) size_val = base_size;
        }

        printf("Generated trace %d: size=%f\n", i, size_val);

        traces[i].traceName = "object";
        traces[i].size = (long long unsigned)(size_val);
        traces[i].size_restore_top = 0;
        for (int rs = 0; rs < MAX_SIZE_RESTORE_STACK; ++rs)
            traces[i].size_restore_stack[rs] = 0.0;
        traces[i].mean_interarrival = traceData[0].inter_arrival; /* use configured mean */
        traces[i].service_time_c = 0.0f;
        traces[i].service_time_h = 0.0f;
        traces[i].service_time_idx = 0.0f;
        traces[i].service_time_ida = 0.0f;
        traces[i].service_time_io = 0.0f;
        traces[i].service_time_app = 0.0f;
        traces[i].MUESTRAS = 1;

        position1 = rand() % configuration->workers;
        position2 = rand() % configuration->workers;

        if (configuration->workers == 1)
            position = position1;
        else
        {
            while (position1 == position2)
                position2 = rand() % configuration->workers;

            if (arrayWorkers[position1].sizeStorage > arrayWorkers[position2].sizeStorage)
                position = position2;
            else
                position = position1;
        }

        arrayWorkers[position].sizeStorage += traces[i].size;
        arrayWorkers[position].input_workload_size += (long)traces[i].size;
        arrayWorkers[position].output_workload_size += (long)traces[i].size;
        arrayWorkers[position].sizeWorker++;
        arrayWorkers[position].trace[contar[position]] = traces[i];
        contar[position]++;

        i++;
    }
    free(contar);
    free(traces);

    return arrayWorkers;
}

/**
 * @brief Function that deploy threads in the pattern.
 */
void deployThread_stages(struct config *configuration, struct worker *arrayWorkers, int stageNumber)
{
    int rc, i;
    pthread_t threads[configuration->workers]; //< thread handles
    int created_indices[configuration->workers];
    int created_count = 0;

    /* Every object batch enters the first task of the first stage. Each manager
       forwards to the next task and then to the next stage. */
    for (i = 0; i < configuration->workers; i++)
    {
        if (arrayWorkers[i].sizeWorker > 0)
        {
            arrayWorkers[i].stage_owner = stageNumber;
            arrayWorkers[i].machine_id = stage_machine_id(stageNumber);
            arrayWorkers[i].service_profile_index = machine_service_profile_index(arrayWorkers[i].machine_id);

            if (nfr_initialized)
            {
                if (configuration && configuration->stages_number > 0 && stageNumber == configuration->stages[0])
                    inc_outstanding();

                if (enqueue_stage_start(stageNumber, &arrayWorkers[i]) != 0)
                {
                    dec_outstanding();
                    printf("Unable to enqueue worker %d into stage %d pipeline\n", arrayWorkers[i].id, stageNumber);
                }
            }
            else
            {
                struct stage_definition *stage_def = global_stage_definition(stageNumber);
                struct nfr_requirement *req = (stage_def && stage_def->input_count > 0) ? &stage_def->input_requirements[0] : NULL;
                /* fallback to direct thread if manager not available */
                arrayWorkers[i].stage = stageNumber;
                arrayWorkers[i].b_fs = stage_filesystem_bandwidth(stageNumber);
                arrayWorkers[i].b_fs_read = stage_filesystem_read_bandwidth(stageNumber);
                arrayWorkers[i].b_fs_write = stage_filesystem_write_bandwidth(stageNumber);
                arrayWorkers[i].pipeline_is_input = 1;
                arrayWorkers[i].task_id = 0;
                arrayWorkers[i].task_type = req ? req->type : NFR_NONE;
                strncpy(arrayWorkers[i].task_name, req ? req->task_name : "task", sizeof(arrayWorkers[i].task_name) - 1);
                arrayWorkers[i].task_name[sizeof(arrayWorkers[i].task_name) - 1] = '\0';
                strncpy(arrayWorkers[i].task_algorithm, req ? req->algorithm : "", sizeof(arrayWorkers[i].task_algorithm) - 1);
                arrayWorkers[i].task_algorithm[sizeof(arrayWorkers[i].task_algorithm) - 1] = '\0';
                rc = pthread_create(&threads[i], NULL, sendWorkstage, (void *)&arrayWorkers[i]);
                if (rc)
                {
                    printf("\tMaster ERROR; return code from pthread_create() is %d\n", rc);
                    exit(-1);
                }
                created_indices[created_count++] = i;
            }
        }
        else
        {
            arrayWorkers[i].stage = 0; /* not running this stage on this worker */
        }
    }

    /* Join only created threads */
    for (i = 0; i < created_count; ++i)
    {
        int idx = created_indices[i];
        pthread_join(threads[idx], NULL);
    }
}

/**
 * @brief Function that send work to workers.
 */
void *sendWorkstage(void *threadarg)
{
    struct worker *my_data;

    my_data = (struct worker *)threadarg; //< Conversion to structure attribute

    serviceTime(my_data);

    printf("Worker %d (machine %d) finished stage %d\n", my_data->id, my_data->machine_id, my_data->stage);

    pthread_exit(NULL); //< Kill thread
}

void serviceTime(struct worker *my_data)
{
    int is_input = my_data->pipeline_is_input ? 1 : 0;
    set_service_time_profile(my_data ? my_data->service_profile_index : 0);
    printf("Worker %d (machine %d) processing stage %d %s task %d (%s:%s)\n",
           my_data->id,
           my_data->machine_id,
           my_data->stage,
           is_input ? "input" : "output",
           my_data->task_id,
           my_data->task_name,
           my_data->task_algorithm);

    switch (my_data->task_type)
    {
    case NFR_COMPRESS:
        if (is_input)
            decompress_time(my_data);
        else
            compress_time(my_data);
        break;
    case NFR_ENCRYPT:
        if (is_input)
            decrypt_time(my_data);
        else
            encrypt_time(my_data);
        break;
    case NFR_HASH:
        if (is_input)
            hash_verify_time(my_data);
        else
            hash_calculate_time(my_data);
        break;
    default:
        break;
    }

    printf("Worker %d (machine %d) completed stage %d %s task %d (%s:%s)\n",
           my_data->id,
           my_data->machine_id,
           my_data->stage,
           is_input ? "input" : "output",
           my_data->task_id,
           my_data->task_name,
           my_data->task_algorithm);
}

static const char *agent_prefix_for_worker(const struct worker *my_data)
{
    return my_data && my_data->pipeline_is_input ? "input_agent" : "output_agent";
}

static void refresh_worker_storage(struct worker *my_data)
{
    long total = 0;
    if (!my_data)
        return;

    for (int j = 0; j < my_data->sizeWorker; ++j)
        total += (long)my_data->trace[j].size;

    my_data->sizeStorage = total;
}

static void trace_push_restore_size(struct traces *trace, double original_size)
{
    if (!trace)
        return;

    if (trace->size_restore_top >= MAX_SIZE_RESTORE_STACK)
    {
        printf("Warning: compression restore stack full for object; decompression may need ratio fallback\n");
        return;
    }

    trace->size_restore_stack[trace->size_restore_top++] = original_size;
}

static int trace_pop_restore_size(struct traces *trace, double *original_size)
{
    if (!trace || !original_size || trace->size_restore_top <= 0)
        return 0;

    trace->size_restore_top--;
    *original_size = trace->size_restore_stack[trace->size_restore_top];
    trace->size_restore_stack[trace->size_restore_top] = 0.0;
    return 1;
}

static int run_queue_estimator(const char *container_prefix, int worker_id, double mean_interarrival, double mean_service, int samples, const char *result_file, double *total_time);

static int record_queue_result(struct worker *my_data, int result_stage, double st_avg, double *total_time)
{
    if (!my_data || my_data->sizeWorker <= 0)
        return -1;

    if (stage_uses_burst_arrivals(my_data->stage))
    {
        if (total_time)
            *total_time = st_avg * (double)my_data->sizeWorker;
        printf("Burst-arrival mode: bypassing queue estimator for worker %d stage %d task %s; raw total %f seconds\n",
               my_data->id,
               my_data->stage,
               my_data->task_name,
               total_time ? *total_time : st_avg * (double)my_data->sizeWorker);
        return 0;
    }

    const char *prefix = agent_prefix_for_worker(my_data);
    char result_file[128];
    snprintf(result_file, sizeof(result_file), "w%d_stage%d.txt", my_data->id, result_stage);

    if (run_queue_estimator(prefix,
                            my_data->id,
                            stage_mean_interarrival_seconds(my_data->stage, my_data),
                            st_avg,
                            my_data->sizeWorker,
                            result_file,
                            total_time) != 0)
    {
        if (total_time)
            *total_time = st_avg * (double)my_data->sizeWorker;
        printf("Warning: queue estimator failed for worker %d stage %d task %s; using raw total %f seconds\n",
               my_data->id,
               my_data->stage,
               my_data->task_name,
               total_time ? *total_time : 0.0);
        return -1;
    }

    return 0;
}

static void apply_queue_total_to_nfr_times(struct worker *my_data, int task_type, const double *raw_times, double raw_total, double queued_total)
{
    if (!my_data || my_data->sizeWorker <= 0 || raw_total <= 0.0 || queued_total < 0.0)
        return;

    for (int j = 0; j < my_data->sizeWorker; ++j)
    {
        double raw_time = raw_times ? raw_times[j] : raw_total / (double)my_data->sizeWorker;
        double queued_time = queued_total * (raw_time / raw_total);
        double delta = queued_time - raw_time;

        switch (task_type)
        {
        case NFR_COMPRESS:
            my_data->trace[j].service_time_c += (float)delta;
            break;
        case NFR_HASH:
            my_data->trace[j].service_time_h += (float)delta;
            break;
        case NFR_ENCRYPT:
            my_data->trace[j].service_time_ida += (float)delta;
            break;
        default:
            break;
        }
    }
}

static void apply_queue_total_to_application_times(struct worker *my_data, const double *raw_times, double raw_total, double queued_total)
{
    if (!my_data || my_data->sizeWorker <= 0 || raw_total <= 0.0 || queued_total < 0.0)
        return;

    for (int j = 0; j < my_data->sizeWorker; ++j)
    {
        double raw_time = raw_times ? raw_times[j] : raw_total / (double)my_data->sizeWorker;
        double queued_time = queued_total * (raw_time / raw_total);
        double delta = queued_time - raw_time;
        my_data->trace[j].service_time_app += (float)delta;
    }
}

static int run_queue_estimator(const char *container_prefix, int worker_id, double mean_interarrival, double mean_service, int samples, const char *result_file, double *total_time)
{
    char line[256];
    double avg_delay = 0.0;
    double avg_queue = 0.0;
    double utilization = 0.0;
    double simulation_time = 0.0;
    int parsed = 0;

    if (!container_prefix || samples <= 0 || mean_service <= 0.0)
        return -1;

    char *path = getenv("PWD");
    if (!path)
        return -1;

    mkdir("results", 0777);

    char *command = build_queue_estimator_command(container_prefix,
                                                  worker_id,
                                                  mean_interarrival,
                                                  mean_service,
                                                  samples);
    if (!command)
        return -1;

    printf("Running queue estimator for worker %d - %s with mean interarrival %f seconds, mean service %f seconds, samples %d\n",
           worker_id,
           container_prefix,
           mean_interarrival,
           mean_service,
           samples);

    FILE *fp = popen(command, "r");
    free(command);
    if (!fp)
        return -1;

    FILE *out = NULL;
    char *result_path = NULL;
    if (result_file && result_file[0] != '\0')
    {
        int result_path_size = snprintf(NULL, 0, "%s/results/%s", path, result_file) + 1;
        result_path = malloc(result_path_size);
        if (result_path)
        {
            snprintf(result_path, result_path_size, "%s/results/%s", path, result_file);
            out = fopen(result_path, "a");
        }
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (out)
            fputs(line, out);
        if (sscanf(line, "%lf %lf %lf %lf", &avg_delay, &avg_queue, &utilization, &simulation_time) == 4)
            parsed = 1;
    }

    if (out)
        fclose(out);
    free(result_path);
    pclose(fp);

    if (!parsed)
        return -1;

    if (total_time)
        *total_time = simulation_time;

    return 0;
}

static double application_time(struct worker *my_data)
{
    if (!my_data || my_data->sizeWorker <= 0)
        return 0.0;

    struct stage_definition *stage_def = global_stage_definition(my_data->stage);
    double avg_service_time = stage_def ? stage_def->application_mean_service_time : 0.0;
    if (stage_def && stage_def->name[0] != '\0') {
        double loaded_time = applicationStageAlgo(stage_def->name);
        if (loaded_time > 0.0) {
            avg_service_time = loaded_time;
        }
    }
    
    double penalty = 1.0;
    if (global_config && global_config->workers > 1) {
        penalty = 1.0 + (global_config->concurrency_penalty * (global_config->workers - 1));
    }
    avg_service_time *= penalty;

    double size_factor = stage_def ? stage_def->application_size_factor : 1.0;
    if (avg_service_time <= 0.0)
        return 0.0;
    if (size_factor <= 0.0)
        size_factor = 1.0;

    char result_file[128];
    snprintf(result_file, sizeof(result_file), "w%d_stage%d_application.txt", my_data->id, my_data->stage);

    double total_time = 0.0;
    double raw_total = 0.0;
    double *raw_times = calloc(my_data->sizeWorker, sizeof(double));
    double mean_interarrival = stage_mean_interarrival_seconds(my_data->stage, my_data);

    for (int j = 0; j < my_data->sizeWorker; ++j)
    {
        double t_read = 0.0, t_write = 0.0;
        long unsigned size_before = my_data->trace[j].size;
        long unsigned size_after = (long unsigned)((double)size_before * size_factor + 0.5);
        double raw_time;

        if (size_after == 0)
            size_after = 1;

        if (my_data->b_fs_read > 0.0)
        {
            t_read = (double)size_before / my_data->b_fs_read;
        }
        if (my_data->b_fs_write > 0.0)
        {
            t_write = (double)size_after / my_data->b_fs_write;
        }

        raw_time = avg_service_time + t_read + t_write;
        my_data->trace[j].service_time_app += (float)raw_time;
        my_data->trace[j].size = size_after;
        raw_total += raw_time;
        if (raw_times)
            raw_times[j] = raw_time;
    }
    refresh_worker_storage(my_data);

    double avg_total_service_time = raw_total / (double)my_data->sizeWorker;

    printf("APPLICATION TIME: Worker %d (machine %d) stage %d application with average service time %f seconds, size factor %f, interarrival time %f seconds; samples %d; running queue estimator...\n",
           my_data->id,
           my_data->machine_id,
           my_data->stage,
           avg_total_service_time,
           size_factor,
           mean_interarrival,
           my_data->sizeWorker);
    if (stage_uses_burst_arrivals(my_data->stage))
    {
        total_time = raw_total;
        printf("Burst-arrival mode: bypassing application queue estimator for worker %d stage %d; raw total %f seconds\n",
               my_data->id,
               my_data->stage,
               total_time);
    }
    else if (run_queue_estimator("application_agent",
                                 my_data->id,
                                 mean_interarrival,
                                 avg_total_service_time,
                                 my_data->sizeWorker,
                                 result_file,
                                 &total_time) != 0)
    {
        total_time = raw_total;
        printf("Warning: application queue estimator failed for worker %d stage %d; using %f seconds fallback\n",
               my_data->id,
               my_data->stage,
               total_time);
    }

    apply_queue_total_to_application_times(my_data, raw_times, raw_total, total_time);
    free(raw_times);

    double per_object_time = total_time / (double)my_data->sizeWorker;
    printf("Worker %d (machine %d) stage %d application estimated total time %f seconds for %d objects (average per object: %f seconds)\n",
           my_data->id,
           my_data->machine_id,
           my_data->stage,
           total_time,
           my_data->sizeWorker,
           per_object_time);

    printf("Worker %d (machine %d) completed stage %d application with average service time %f seconds and simulated total time %f seconds\n",
           my_data->id,
           my_data->machine_id,
           my_data->stage,
           avg_total_service_time,
           total_time);

    return total_time;
}

void compress_time(struct worker *my_data)
{
    int j;
    double st_sum, st_avg;

    st_sum = 0;
    st_avg = 0;

    if (my_data->sizeWorker > 0)
    {
        double *raw_times = calloc(my_data->sizeWorker, sizeof(double));
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            double t_read = 0.0, t_write = 0.0;
            long unsigned size_before = my_data->trace[j].size;
            printf("AAAAAAAAAAAAAAAA Worker %d (machine %d) compressing object %d of size %.2f MB with algorithm %s\n",
                   my_data->id, my_data->machine_id, j, (double)size_before / 1048576.0, my_data->task_algorithm);
            if (my_data->b_fs_read > 0.0)
                t_read = (double)size_before / my_data->b_fs_read;

            float comp_time = compressStageAlgo((double)size_before, my_data->task_algorithm);
            if (comp_time <= 0.0f)
                comp_time = compressStage((double)size_before);
            long unsigned new_size = (long unsigned)compressStageSizeAlgo((double)size_before, my_data->task_algorithm);
            if (my_data->b_fs_write > 0.0)
                t_write = (double)new_size / my_data->b_fs_write;

            double raw_time = comp_time + t_read + t_write;
            my_data->trace[j].service_time_c += (float)raw_time;
            trace_push_restore_size(&my_data->trace[j], (double)size_before);
            my_data->trace[j].size = new_size;
            if (raw_times)
                raw_times[j] = raw_time;
            st_sum += raw_time;
            printf("****Worker %d (machine %d) compressed object %d from %.2f MB to %f MB in %.2f seconds (I/O time: %.2f seconds)\n",
                   my_data->id, my_data->machine_id, j, (double)size_before / 1048576.0, (double)new_size / 1048576.0, comp_time, (float)(t_read + t_write));
        }
        st_avg = st_sum / my_data->sizeWorker;
    
        printf("------Worker %d (machine %d) completed compression stage with average time %.2f seconds\n", my_data->id, my_data->machine_id, st_avg);
        refresh_worker_storage(my_data);
        double queued_total = st_sum;
        record_queue_result(my_data, 1, st_avg, &queued_total);
        apply_queue_total_to_nfr_times(my_data, NFR_COMPRESS, raw_times, st_sum, queued_total);
        free(raw_times);
    }
}

void hashing_time(struct worker *my_data)
{
    hash_calculate_time(my_data);
}

static void hash_task_time(struct worker *my_data, int result_stage)
{
    int j;
    double st_sum, st_avg;

    st_sum = 0;
    st_avg = 0;

    if (my_data->sizeWorker > 0)
    {
        double *raw_times = calloc(my_data->sizeWorker, sizeof(double));
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            double t_read = 0.0, t_write = 0.0;
            long unsigned size_before = my_data->trace[j].size;
            if (my_data->b_fs_read > 0.0)
                t_read = (double)size_before / my_data->b_fs_read;

            float hash_time = hashingStageAlgo((double)size_before, my_data->task_algorithm);
            if (hash_time <= 0.0f)
                hash_time = hashingStage((double)size_before);
            if (my_data->b_fs_write > 0.0)
                t_write = (double)size_before / my_data->b_fs_write;

            double raw_time = hash_time + t_read + t_write;
            my_data->trace[j].service_time_h += (float)raw_time;
            if (raw_times)
                raw_times[j] = raw_time;
            st_sum += raw_time;
        }
        st_avg = st_sum / my_data->sizeWorker;
        double queued_total = st_sum;
        record_queue_result(my_data, result_stage, st_avg, &queued_total);
        apply_queue_total_to_nfr_times(my_data, NFR_HASH, raw_times, st_sum, queued_total);
        free(raw_times);
    }
}

void hash_calculate_time(struct worker *my_data)
{
    hash_task_time(my_data, 2);
}

void hash_verify_time(struct worker *my_data)
{
    hash_task_time(my_data, 2);
}

static void crypto_task_time(struct worker *my_data, int is_decrypt)
{
    int j;
    double st_sum, st_avg;

    st_sum = 0;
    st_avg = 0;

    if (my_data->sizeWorker > 0)
    {
        double *raw_times = calloc(my_data->sizeWorker, sizeof(double));
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            double t_read = 0.0, t_write = 0.0;
            long unsigned size_before = my_data->trace[j].size;
            if (my_data->b_fs_read > 0.0)
            {
                t_read = (double)size_before / my_data->b_fs_read;
            }
            if (my_data->b_fs_write > 0.0)
            {
                t_write = (double)size_before / my_data->b_fs_write;
            }

            float crypto_time = is_decrypt ? IDADecodeStageAlgo((double)size_before, my_data->task_algorithm) : IDAStageAlgo((double)size_before, my_data->task_algorithm);
            if (crypto_time <= 0.0f)
                crypto_time = is_decrypt ? IDADecodeStage((double)size_before) : IDAStage((double)size_before);
            if (crypto_time <= 0.0f)
                crypto_time = hashingStage((double)size_before);

            double raw_time = crypto_time + t_read + t_write;
            my_data->trace[j].service_time_ida += (float)raw_time;
            if (raw_times)
                raw_times[j] = raw_time;
            st_sum += raw_time;
        }

        st_avg = st_sum / my_data->sizeWorker;
        double queued_total = st_sum;
        record_queue_result(my_data, 4, st_avg, &queued_total);
        apply_queue_total_to_nfr_times(my_data, NFR_ENCRYPT, raw_times, st_sum, queued_total);
        free(raw_times);
    }
}

void encrypt_time(struct worker *my_data)
{
    crypto_task_time(my_data, 0);
}

void decrypt_time(struct worker *my_data)
{
    crypto_task_time(my_data, 1);
}

void indexing_time(struct worker *my_data)
{
    char *command, *path;
    int j;
    float st, st_avg;

    st = 0;
    st_avg = 0;
    my_data->service_time = 0;
    path = getenv("PWD");

    if (my_data->sizeWorker > 0)
    {
        st = indexingStage(my_data->sizeWorker);
        st_avg = st / my_data->sizeWorker;

        /* Add I/O: read each object before indexing and write result after indexing */
        double io_total = 0.0;
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            if (my_data->b_fs_read > 0.0)
            {
                double t_read = (double)my_data->trace[j].size / my_data->b_fs_read;
                double t_write = my_data->b_fs_write > 0.0 ? (double)my_data->trace[j].size / my_data->b_fs_write : 0.0; /* metadata write approx same size */
                io_total += (t_read + t_write);
            }
        }

        st_avg += (float)(io_total / my_data->sizeWorker);

        char result_path[1024];
        snprintf(result_path, sizeof(result_path), "%s/results/w%d_stage3.txt", path, my_data->id);
        command = build_queue_estimator_redirect_command(agent_container_prefix,
                                                         my_data->id,
                                                         stage_mean_interarrival_seconds(my_data->stage, my_data),
                                                         st_avg,
                                                         my_data->sizeWorker,
                                                         result_path);

        /* record per-worker total and per-object indexing times */
        my_data->service_time = st + (float)io_total;
        for (j = 0; j < my_data->sizeWorker; ++j) {
            my_data->trace[j].service_time_idx = st_avg; /* per-object average for indexing */
        }
        if (command)
        {
            execute_command(command);
            free(command);
        }
    }
}

void IDA_time(struct worker *my_data)
{
    int j;
    char *command, *path;
    long unsigned newSize;
    float st_sum, st_avg;

    st_sum = 0;
    st_avg = 0;
    newSize = 0;

    path = getenv("PWD");

    if (my_data->sizeWorker > 0)
    {
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            double t_read = 0.0, t_write = 0.0;
            long unsigned size_before = my_data->trace[j].size;
            if (my_data->b_fs_read > 0.0)
                t_read = (double)size_before / my_data->b_fs_read;

            float ida_time = IDAStage((double)size_before);
            long unsigned new_size = (long unsigned)IDAStageSize((double)size_before);
            if (my_data->b_fs_write > 0.0)
                t_write = (double)new_size / my_data->b_fs_write;

            my_data->trace[j].service_time_ida = ida_time + (float)(t_read + t_write);
            my_data->trace[j].size = new_size;
            st_sum += my_data->trace[j].service_time_ida;
        }
        st_avg = st_sum / my_data->sizeWorker;

        char result_path[1024];
        snprintf(result_path, sizeof(result_path), "%s/results/w%d_stage4.txt", path, my_data->id);
        command = build_queue_estimator_redirect_command(agent_container_prefix,
                                                         my_data->id,
                                                         stage_mean_interarrival_seconds(my_data->stage, my_data),
                                                         st_avg,
                                                         my_data->sizeWorker,
                                                         result_path);

        if (command)
        {
            execute_command(command);
            free(command);
        }
    }
}

/**
 * @brief Input agent: reconstruct (IDA inverse) — shrinks data back.
 * Uses the same interpolated time but applies the inverse size ratio (k/(k+m)).
 */
void IDA_reconstruct_time(struct worker *my_data)
{
    int j;
    char *command, *path;
    float st_sum, st_avg;

    st_sum = 0;
    st_avg = 0;

    path = getenv("PWD");

    if (my_data->sizeWorker > 0)
    {
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            double t_read = 0.0, t_write = 0.0;
            long unsigned size_before = my_data->trace[j].size;
            if (my_data->b_fs_read > 0.0)
                t_read = (double)size_before / my_data->b_fs_read;

            float dec_time = compressStage((double)size_before);
            /* Restore size using existing logic */
            long compressed = compressStageSize(my_data->trace[j].size);
            long unsigned new_size = size_before;
            if (compressed > 0)
                new_size = (long unsigned)((double)my_data->trace[j].size * my_data->trace[j].size / (double)compressed);

            if (my_data->b_fs_write > 0.0)
                t_write = (double)new_size / my_data->b_fs_write;

            my_data->trace[j].service_time_c = dec_time + (float)(t_read + t_write);
            my_data->trace[j].size = new_size;
            st_sum += my_data->trace[j].service_time_c;
        }
        st_avg = st_sum / my_data->sizeWorker;

        char result_path[1024];
        snprintf(result_path, sizeof(result_path), "%s/results/w%d_stage4.txt", path, my_data->id);
        command = build_queue_estimator_redirect_command(agent_container_prefix,
                                                         my_data->id,
                                                         stage_mean_interarrival_seconds(my_data->stage, my_data),
                                                         st_avg,
                                                         my_data->sizeWorker,
                                                         result_path);

        if (command)
        {
            execute_command(command);
            free(command);
        }
    }
}

/**
 * @brief Input agent: decompress — restores the size recorded by the matching compression task.
 */
void decompress_time(struct worker *my_data)
{
    int j;
    double st_sum, st_avg;

    st_sum = 0;
    st_avg = 0;

    if (my_data->sizeWorker > 0)
    {
        double *raw_times = calloc(my_data->sizeWorker, sizeof(double));
        for (j = 0; j < my_data->sizeWorker; ++j)
        {
            double t_read = 0.0, t_write = 0.0;
            long unsigned size_before = my_data->trace[j].size;
            if (my_data->b_fs_read > 0.0)
                t_read = (double)size_before / my_data->b_fs_read;

            float dec_time = decompressStageAlgo((double)size_before, my_data->task_algorithm);
            if (dec_time <= 0.0f)
                dec_time = compressStage((double)size_before);
            printf("****Worker %d (machine %d) decompressing object %d of size %.2f MB with algorithm %s\n",
                   my_data->id, my_data->machine_id, j, (double)size_before / 1048576.0, my_data->task_algorithm);
            long unsigned new_size = size_before;
            double restored_size = 0.0;
            if (trace_pop_restore_size(&my_data->trace[j], &restored_size))
            {
                new_size = (long unsigned)(restored_size + 0.5);
            }
            else
            {
                long compressed = compressStageSizeAlgo(my_data->trace[j].size, my_data->task_algorithm);
                if (compressed > 0)
                    new_size = (long unsigned)((double)my_data->trace[j].size * my_data->trace[j].size / (double)compressed);
                printf("Warning: decompressing object %d without a recorded compression size; using ratio fallback\n", j);
            }

            if (my_data->b_fs_write > 0.0)
                t_write = (double)new_size / my_data->b_fs_write;

            double raw_time = dec_time + t_read + t_write;
            my_data->trace[j].service_time_c += (float)raw_time;
            my_data->trace[j].size = new_size;
            if (raw_times)
                raw_times[j] = raw_time;
            st_sum += raw_time;
        }
        st_avg = st_sum / my_data->sizeWorker;
        refresh_worker_storage(my_data);

        printf("Worker %d - Decompress stage: avg service time = %f seconds\n", my_data->id, st_avg);

        double queued_total = st_sum;
        record_queue_result(my_data, 1, st_avg, &queued_total);
        apply_queue_total_to_nfr_times(my_data, NFR_COMPRESS, raw_times, st_sum, queued_total);
        free(raw_times);
    }
}

void upload_time(struct worker *my_data)
{
    int j;
    char *command, *path;

    path = getenv("PWD");
    for (j = 0; j < my_data->sizeWorker; ++j)
    {
        double t_write = 0.0;
        long unsigned size_out = my_data->trace[j].size;
        if (my_data->b_fs_write > 0.0)
            t_write = (double)size_out / my_data->b_fs_write;

        my_data->trace[j].service_time_io = (float)t_write;

        char result_path[1024];
        snprintf(result_path, sizeof(result_path), "%s/results/w%d_stage5.txt", path, my_data->id);
        command = build_queue_estimator_redirect_command(agent_container_prefix,
                                                         my_data->id,
                                                         stage_mean_interarrival_seconds(my_data->stage, my_data),
                                                         my_data->trace[j].service_time_io,
                                                         my_data->trace[j].MUESTRAS,
                                                         result_path);

        if (command)
        {
            execute_command(command);
            free(command);
        }
    }
}

/*command = malloc(strlen(getenv("PWD")) + 50 *sizeof(char));

strcpy(command, "");
sprintf( command,"mkdir -p %s/FilesCompress", getenv("PWD") );

fp = popen(command, "r");
if (fp == NULL) {
   printf("Failed to run command\n" );
   exit(1);
}
pclose(fp);

free(command);


command = malloc(strlen(getenv("PWD")) + 50 *sizeof(char));
strcpy(command, "");
sprintf( command,"chmod 777 -R %s/FilesCompress", getenv("PWD") );

fp = popen(command, "r");
if (fp == NULL) {
   printf("Failed to run command\n" );
   exit(1);
}
pclose(fp);
free(command);*/
