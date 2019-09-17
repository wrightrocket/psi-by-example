#include <argp.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* kernel accepts window sizes ranging from 500ms to 10s */
#define CPU_TRACKING_WINDOW_MS      500     // 0.5 seconds
#define IO_TRACKING_WINDOW_MS       1000    // 1 second
#define MEMORY_TRACKING_WINDOW_MS   750     // 0.75 seconds 
#define MS_TO_US                    1000    // millisecs to microseconds factor

/* min monitoring update interval is 50ms and max is 1s */
#define CPU_TRIGGER_THRESHOLD_MS    50      // 50 ms
#define IO_TRIGGER_THRESHOLD_MS     100     // 0.1 seconds
#define MEMORY_TRIGGER_THRESHOLD_MS 75      // 75 ms

/* paths to the pressure stall information files writable by kernel 5.2+ */
#define CPU_PRESSURE_FILE           "/proc/pressure/cpu"
#define IO_PRESSURE_FILE            "/proc/pressure/io"
#define MEMORY_PRESSURE_FILE        "/proc/pressure/memory"

/* index values to refer to each of the pressure files */
#define FD_CPU_IDX                  0
#define FD_IO_IDX                   1
#define FD_MEMORY_IDX               2

/* format values to refer to '%T' vs '%s' time formats */
#define FMT_YMD_HMS                 0
#define FMT_EPOCH                   1

/* size values for the length of different char arrays */
#define SZ_IDX                      3
#define SZ_CONTENT                  128
#define SZ_EVENT                    256
#define SZ_TIME                     21
#define SZ_EPOCH                    11

/* errors defined to differentiate program exit values */
#define ERROR_KERNEL_UNSUPPORTED    1
#define ERROR_PRESSURE_OPEN         2 
#define ERROR_PRESSURE_WRITE        3 
#define ERROR_PRESSURE_POLL_FDS     4
#define ERROR_PRESSURE_FILE_GONE    5
#define ERROR_PRESSURE_EVENT_UNK    6   

/* global variables for polling file descriptor, char and int arrays */
const char *argp_program_version =  "psi 1.0";
const char *argp_program_bug_address =  "<keith.wright@linuxacademy.comg>";
/* Program documentation. */
static char doc[] =  "psi - Pressure Stall Information(PSI) performance tool";
static struct argp argp = { 0, 0, 0, doc };

struct pollfd fds[SZ_IDX];
char content_str[SZ_CONTENT];
char *pressure_file[SZ_IDX];
char time_str[SZ_TIME];
int delay_threshold_ms[SZ_IDX];
int tracking_window_ms[SZ_IDX];
int continue_event_loop = 1;

/* close all file descriptors before exiting */
void close_fds(){
    fprintf(stderr, "Please wait until all three file descriptors are closed\n");
    for (int i=0; i < SZ_IDX; i++){
        fprintf(stderr, "Closing file descriptor fds[%i] for %s\n", i, pressure_file[i]);
        usleep(tracking_window_ms[i] * MS_TO_US);
        close(fds[i].fd);
    }
    fprintf(stderr, "\nAll file descriptors now closed, exiting now!\n");
}

/* signal handler for SIGINT and SIGTERM */
void sig_handler(int sig_num) { 
    /* Reset handler to catch SIGINT next time. 
       Refer http://en.cppreference.com/w/c/program/signal */
    printf("\nTerminating in response to Ctrl+C \n"); 
    signal(SIGINT, sig_handler); 
    signal(SIGTERM, sig_handler); 
    continue_event_loop = 0; 
    close_fds(); 
    fflush(stdout); 
}

/* set the global time_str char array to the ISO formatted time */
void set_time_str(int fmt) {
    time_t now;
    struct tm* tm_info;

    time(&now);
    tm_info = localtime(&now);
    strftime(time_str, SZ_TIME, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* set the global content_str char array to the contents of a PSI file */
void set_content_str(int psi_idx) {
    int fd;

    memset(&(content_str[0]), 0, SZ_CONTENT);
    fd = open(pressure_file[psi_idx], O_NONBLOCK | O_RDONLY);
    read(fd, content_str, SZ_CONTENT);
    close(fd);
}

/* set the event delay for `some delay_threshold_ms tracking_window_ms`
 * so tasks delayed greater than the threshold time within a tracking window
 * will cause an event indicating that condition of distress
*/
void poll_pressure_events() {
    char distress_event[SZ_EVENT];

    for (int i = 0; i < SZ_IDX; i++) {

        memset(&(distress_event[0]), 0, SZ_EVENT);
        fds[i].fd = open(pressure_file[i], O_RDWR | O_NONBLOCK);
        if (fds[i].fd < 0) {
            fprintf(stderr, "Error open() pressure file %s:", pressure_file[i]);
            exit(ERROR_PRESSURE_OPEN);
        }
        fds[i].events = POLLPRI;
        snprintf(distress_event, SZ_EVENT, "some %d %d",
                delay_threshold_ms[i] * MS_TO_US,
                tracking_window_ms[i] * MS_TO_US);
        printf("\n%s distress_event:\n%s\n", pressure_file[i], distress_event);
        set_content_str(i);
        printf("%s content:\n%s\n", pressure_file[i], content_str);
        if (write(fds[i].fd, distress_event, strlen(distress_event) + 1) < 0) {
            fprintf(stderr, "Error write() pressure file: %s\n",
                    pressure_file[i]);
            exit(ERROR_PRESSURE_WRITE);
        }
    }
}

/* loop until program is terminated or interrupted
 * increment event_counter for cpu, io, and memory
 * continue polling for events until continue_event_loop
 * changes value */
void pressure_event_loop() {
    int event_counter[SZ_IDX];

    for (int i = 0; i < SZ_IDX; i++) {
        event_counter[i] = 1;
    }

    printf("\nPolling for events...\n");
    while (continue_event_loop == 1) {
        int n = poll(fds, SZ_IDX, -1);
        if (continue_event_loop == 0) break;
        if (n < 0) {
            fprintf(stderr, "\nError using poll() function\n");
            exit(ERROR_PRESSURE_POLL_FDS);
        }

        for (int i = 0; i < SZ_IDX; i++) {
            if ((fds[i].revents == 0) || (continue_event_loop == 0)) {
                continue;
            }
            if (fds[i].revents & POLLERR) {
                fprintf(stderr, "\nError: poll() event source is gone.\n");
                exit(ERROR_PRESSURE_FILE_GONE);
            }
            if (fds[i].events) {
                set_time_str(FMT_YMD_HMS);
                set_content_str(i);
                printf("%s %i %s %s\n", pressure_file[i], event_counter[i]++, time_str, content_str);
            } else {
                fprintf(stderr, "\nUnrecognized event: 0x%x.\n", fds[i].revents);
                exit(ERROR_PRESSURE_EVENT_UNK);
            }
        }
    }
}

void verify_proc_pressure() {
    struct stat st;
    int sret = stat(CPU_PRESSURE_FILE, &st);

    if (sret == -1) {
        fprintf(stderr,
                "To monitor with poll() in Linux, uname -r must report a kernel version of 5.2+\n");
        exit(ERROR_KERNEL_UNSUPPORTED);
    } else {
        set_time_str(FMT_YMD_HMS);
        printf("Polling events starting at %s", time_str);
    }
}

void populate_arrays() {
    pressure_file[0] = "/proc/pressure/cpu";
    pressure_file[1] = "/proc/pressure/io";
    pressure_file[2] = "/proc/pressure/memory";
    delay_threshold_ms[0] = CPU_TRIGGER_THRESHOLD_MS;
    delay_threshold_ms[1] = IO_TRIGGER_THRESHOLD_MS;
    delay_threshold_ms[2] = MEMORY_TRIGGER_THRESHOLD_MS;
    tracking_window_ms[0] = CPU_TRACKING_WINDOW_MS;
    tracking_window_ms[1] = IO_TRACKING_WINDOW_MS;
    tracking_window_ms[2] = MEMORY_TRACKING_WINDOW_MS;
}

int main(int argc, char **argv) {
    argp_parse (&argp, argc, argv, 0, 0, 0);
    populate_arrays();
    verify_proc_pressure();
    poll_pressure_events();
    signal(SIGTERM, sig_handler); 
    signal(SIGINT, sig_handler);
    pressure_event_loop();
}
