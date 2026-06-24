/**
 * @file simulator.h
 * @mainpage Simulator to Preparation and retrieval service
 * @author Diana E. Carrizales-Espinoza
 * @date November 2019
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <libgen.h>
#include "string.h"

struct config;
void load_service_times(struct config *configuration);
void load_service_times_with_base(struct config *configuration, const char *runtime_base_dir);
void set_service_time_profile(int profile_index);

float interpolation( float x, float x0, float x1, float y0, float y1) ;

float compressStage (long unsigned filesize) ;
float decompressStage (long unsigned filesize) ;
float compressStageAlgo (long unsigned filesize, const char *algo) ;
float decompressStageAlgo (long unsigned filesize, const char *algo) ;

double compressStageSize ( double filesize ) ;
double compressStageSizeAlgo ( double filesize, const char *algo ) ;

float hashingStage (double filesize) ;
double hashingStageSize (double filesize) ;
float hashingStageAlgo (double filesize, const char *algo) ;
double hashingStageSizeAlgo (double filesize, const char *algo) ;

float indexingStage (long numFiles) ;

float IDAStage (double filesize) ;
float IDADecodeStage (double filesize) ;
float IDAStageAlgo (double filesize, const char *algo) ;
float IDADecodeStageAlgo (double filesize, const char *algo) ;
double IDAStageSize (double filesize) ;
double IDAStageSizeAlgo (double filesize, const char *algo) ;

void print_interpolation_points();
float applicationStageAlgo (const char *task) ;

//float uploadStage (long long unsigned filesize) ;
