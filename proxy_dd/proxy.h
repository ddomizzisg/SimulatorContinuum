#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include "service_time.h"
#include "cJSON.h"

#define MAX_SIZE_RESTORE_STACK 16

// STRUCTS DECLARATION
struct traces
{
	char *traceName;
	double size;
	double size_restore_stack[MAX_SIZE_RESTORE_STACK];
	int size_restore_top;
	float mean_interarrival;
	float service_time_c;
	float service_time_h;
	float service_time_idx; /* indexing */
	float service_time_ida; /* IDA/reconstruct */
	float service_time_io;
	float service_time_app;
	int MUESTRAS;
};

struct traceConfig
{
	long long unsigned MUESTRAS;
	float inter_arrival;
	long long unsigned DISTRIBUTION;
	float mean;
	float stddev;
	float SIZE;
	float stddevS;
	long long unsigned Concurrency;
};

/* Distributed continuum: machines and links */
#define MAX_MACHINES 32
#define MAX_LINKS 128
#define MAX_STAGES 10
#define MAX_PIPELINE_TASKS 10
#define INPUT_TASKS MAX_PIPELINE_TASKS
#define OUTPUT_TASKS MAX_PIPELINE_TASKS

#define NFR_NONE 0
#define NFR_COMPRESS 1
#define NFR_ENCRYPT 2
#define NFR_HASH 3
#define NFR_COUNT 4

struct nfr_requirement
{
	int type;
	char task_name[32];
	char algorithm[32];
};

struct stage_definition
{
	int stage;
	char name[32];
	struct nfr_requirement input_requirements[MAX_PIPELINE_TASKS];
	int input_count;
	int input_explicit;
	struct nfr_requirement output_requirements[MAX_PIPELINE_TASKS];
	int output_count;
	double mean_interarrival; /* optional per-stage mean interarrival in seconds */
	int burst_arrival_mode; /* when set, bypass queue estimator and use raw per-worker totals */
	double b_fs; /* legacy symmetric filesystem bandwidth bytes/sec for this stage */
	double b_fs_read; /* filesystem read bandwidth bytes/sec for this stage */
	double b_fs_write; /* filesystem write bandwidth bytes/sec for this stage */
	double application_mean_service_time; /* average application execution time in seconds */
	double application_size_factor; /* multiplicative change in object size caused by the application */
};

struct machine_node
{
	char name[64];
	int stages[10];
	int stages_number;
	char hardware_profile[64];
	char real_values_dir[512];
	int service_profile_index;
};

struct link_node
{
	char from[64];
	char to[64];
	double b_net;	   /* bytes/sec */
	double latency_ms; /* optional */
	/* runtime metrics */
	double bytes_transferred;
	int transfers_count;
	double total_transfer_time; /* seconds */
};

struct worker
{
	int id;
	int sizeWorker;
	long sizeStorage;
	long input_workload_size;
	long output_workload_size;
	int interarrive;
	int stage;
	int stage_owner;		 /* Current stage this worker batch is assigned to (1..10). */
	int machine_id;			 /* Assigned machine index, -1 if local/not set */
	int service_profile_index; /* Loaded interpolation profile for this worker's active machine. */
	char agent_type[16];	 /**< "output" or "input" */
	int pipeline_is_input;	 /**< Current pipeline: 1=input/acquisition, 0=output/delivery. */
	int task_id;			 /**< Current task index inside the active pipeline. */
	int task_type;			 /**< Current NFR type: compression, encryption, or hashing. */
	char task_name[32];		 /**< Current task name for logs and metrics. */
	char task_algorithm[32]; /**< Algorithm selected for the current task. */
	struct traces *trace;
	float service_time;
	double b_fs; /* legacy symmetric filesystem bandwidth bytes/sec for this worker */
	double b_fs_read; /* filesystem read bandwidth bytes/sec for this worker */
	double b_fs_write; /* filesystem write bandwidth bytes/sec for this worker */
	double stage_input_time[MAX_STAGES];
	double stage_output_time[MAX_STAGES];
	double stage_application_time[MAX_STAGES];
	double stage_transfer_time[MAX_STAGES];
	long stage_input_size[MAX_STAGES];
	long stage_output_size[MAX_STAGES];
	double stage_nfr_input_time[MAX_STAGES][NFR_COUNT];
	double stage_nfr_output_time[MAX_STAGES][NFR_COUNT];
	double stage_input_requirement_time[MAX_STAGES][MAX_PIPELINE_TASKS];
	double stage_output_requirement_time[MAX_STAGES][MAX_PIPELINE_TASKS];
	long stage_input_requirement_input_size[MAX_STAGES][MAX_PIPELINE_TASKS];
	long stage_input_requirement_output_size[MAX_STAGES][MAX_PIPELINE_TASKS];
	long stage_output_requirement_input_size[MAX_STAGES][MAX_PIPELINE_TASKS];
	long stage_output_requirement_output_size[MAX_STAGES][MAX_PIPELINE_TASKS];
};

/**
 * @brief Config structure.
 *
 * Config structure stores data of the upload service configuration.
 */
struct config
{
	int workers;										   /**< Number of workers.*/
	int traces_number;									   /**< Numer of traces.*/
	char *traces_fileName;								   /**< FileName of traces configurations.*/
	char agent_type[16];								   /**< Type of agent pipeline: output or input.*/
	int stages[10];										   /**< Chained stages to execute.*/
	int stages_number;									   /**< Number of stages.*/
	struct stage_definition stage_definitions[MAX_STAGES]; /**< Per-stage configurable NFR pipelines. */
	char compression_algo[32];							   /**< Compression algorithm.*/
	char hashing_algo[32];								   /**< Hashing algorithm.*/
	char ida_algo[32];									   /**< IDA algorithm.*/
	char service_time_model[32];							   /**< Service-time model: linear or log-log. */
	char container_platform[32];							   /**< Container platform: docker or apptainer. */
	char queue_container_image[512];						   /**< Queue estimator container image/SIF. */
	char trace_container_image[512];						   /**< Trace generator container image/SIF. */
	char trace_generator_binary[512];						   /**< Native trace generator fallback. */
	int ida_k;											   /**< IDA k_datos.*/
	int ida_m;											   /**< IDA m_paridad.*/
	int aes_key_bits;								   /**< AES key size in bits for confidentiality.*/
	char real_values_dir[512];							   /**< Default service-time dataset directory. */
	double b_fs;										   /**< Legacy symmetric filesystem bandwidth (bytes/sec). */
	double b_fs_read;									   /**< Filesystem read bandwidth (bytes/sec). */
	double b_fs_write;									   /**< Filesystem write bandwidth (bytes/sec). */
	double application_mean_service_time;				   /**< Average application execution time in seconds. */
	double concurrency_penalty;							   /**< Penalty multiplier per concurrent worker. */
	struct machine_node machines[MAX_MACHINES];			   /**< Optional distributed machines */
	int machines_number;
	struct link_node links[MAX_LINKS]; /**< Links between machines */
	int links_number;
};

/**
 * @brief Function returns error in the case to occur.
 * @param s Char string that contains the error.
 * @return Return the error.
 */
void error(const char *s);

struct config *read_config(const char *file_name);

struct traceConfig *read_configTrace(int numberTrace, char *fileName);

int has_inline_traces(void);

void execute_command(char *command);

int is_container_platform_name(const char *value);
void configure_container_runtime(struct config *configuration);

void makeTraceGenerator();

void makeContainers(struct config *configuration);
void makeAgents(int workers, const char *agent_type);

extern char agent_container_prefix[32];

void traceGenerator(struct traceConfig *traceData, int numberTraces);

long fileSize(char *fname);

struct worker *assignation(struct config *configuration, struct traceConfig *traceData);

void deployThread_stages(struct config *configuration, struct worker *arrayWorkers, int stageNumber);

void *sendWorkstage(void *threadarg);

void serviceTime(struct worker *my_data);

void compress_time(struct worker *my_data);

void decompress_time(struct worker *my_data);

void hashing_time(struct worker *my_data);
void hash_calculate_time(struct worker *my_data);
void hash_verify_time(struct worker *my_data);

void encrypt_time(struct worker *my_data);

void decrypt_time(struct worker *my_data);

void indexing_time(struct worker *my_data);

void IDA_time(struct worker *my_data);

void IDA_reconstruct_time(struct worker *my_data);

void upload_time(struct worker *my_data);

/* NFR manager/worker scaffolding */
struct nfr_job
{
	struct worker *w;
};

struct nfr_manager
{
	int stage;				 /* stage number this manager handles */
	int task_id;			 /* task index inside pipeline (0..N-1) */
	int task_type;			 /* NFR_* value */
	char task_name[32];		 /* human-readable task name */
	char task_algorithm[32]; /* selected algorithm for this task */
	pthread_t *threads;		 /* pool of worker threads */
	int num_threads;
	struct nfr_job *queue;
	int q_head;
	int q_tail;
	int q_count;
	int q_size;
	pthread_mutex_t lock;
	pthread_cond_t cond_nonempty;
	pthread_cond_t cond_nonfull;
	int stop;
	int is_input; /* 1=input pipeline, 0=output pipeline */
	int is_application; /* 1=application step between input and output pipelines */
	/* runtime metrics */
	long jobs_processed;
	double total_processing_time; /* seconds */
	double total_simulated_time;	 /* seconds reported by the queue/service model */
};
int nfr_manager_init(struct nfr_manager *m, int stage, int task_id, int task_type, const char *task_name, const char *task_algorithm, int num_threads, int q_size, int is_input, int is_application);
int nfr_manager_enqueue(struct nfr_manager *m, struct worker *w);
void nfr_manager_shutdown(struct nfr_manager *m);
void shutdown_and_report_metrics(struct config *configuration);
int wait_for_managers_empty(struct config *configuration, int timeout_seconds);
int wait_for_outstanding_zero(int timeout_seconds);
