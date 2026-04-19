/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 8192
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 32
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char final_reason[64];
    char log_path[PATH_MAX];

    int pipe_read_fd;
    pthread_t producer_thread;
    int producer_started;

    pthread_mutex_t wait_lock;
    pthread_cond_t wait_cv;

    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_code;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    container_record_t *rec;
} producer_arg_t;

static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static void ensure_log_dir(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) != 0)
        mkdir(LOG_DIR, 0755);
}

static void format_time(time_t t, char *buf, size_t n)
{
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm_info);
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx,
                                                 const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, CONTAINER_ID_LEN) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx,
                                                        pid_t pid)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (cur->host_pid == pid)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if ((cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) &&
            strcmp(cur->rootfs, rootfs) == 0)
            return 1;
        cur = cur->next;
    }
    return 0;
}

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

static int unregister_from_monitor(int monitor_fd,
                                   const char *container_id,
                                   pid_t host_pid)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static void set_final_status(container_record_t *rec, int status)
{
    if (WIFEXITED(status)) {
        rec->state = CONTAINER_EXITED;
        rec->exit_code = WEXITSTATUS(status);
        rec->exit_signal = 0;
        snprintf(rec->final_reason, sizeof(rec->final_reason), "normal_exit");
    } else if (WIFSIGNALED(status)) {
        rec->exit_code = 128 + WTERMSIG(status);
        rec->exit_signal = WTERMSIG(status);

        if (rec->stop_requested) {
            rec->state = CONTAINER_STOPPED;
            snprintf(rec->final_reason, sizeof(rec->final_reason), "stopped");
        } else if (WTERMSIG(status) == SIGKILL) {
            rec->state = CONTAINER_KILLED;
            snprintf(rec->final_reason, sizeof(rec->final_reason), "hard_limit_killed");
        } else {
            rec->state = CONTAINER_KILLED;
            snprintf(rec->final_reason, sizeof(rec->final_reason), "signaled");
        }
    }
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *rec;

        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_by_pid_locked(ctx, pid);
        if (rec) {
            set_final_status(rec, status);
            unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);

            pthread_mutex_lock(&rec->wait_lock);
            pthread_cond_broadcast(&rec->wait_cv);
            pthread_mutex_unlock(&rec->wait_lock);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        int fd;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            (void)write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

static void *producer_thread_main(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    supervisor_ctx_t *ctx = parg->ctx;
    container_record_t *rec = parg->rec;
    char buf[LOG_CHUNK_SIZE];

    while (1) {
        ssize_t n = read(rec->pipe_read_fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, rec->id, sizeof(item.container_id) - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);

        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0)
            break;
    }

    if (rec->pipe_read_fd >= 0) {
        close(rec->pipe_read_fd);
        rec->pipe_read_fd = -1;
    }

    free(parg);
    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    sethostname(cfg->id, strlen(cfg->id));

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        perror("mount-private");

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount proc");

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("exec");
    return 127;
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           container_record_t **out_rec,
                           control_response_t *resp)
{
    int pipefd[2];
    child_config_t *cfg;
    void *stack;
    pid_t pid;
    container_record_t *rec;
    producer_arg_t *parg;

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container id already exists: %s", req->container_id);
        return -1;
    }

    if (rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "rootfs already in use by a running container: %s", req->rootfs);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    ensure_log_dir();

    if (pipe(pipefd) != 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "pipe failed: %s", strerror(errno));
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]);
        close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "calloc failed");
        return -1;
    }

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "malloc failed");
        return -1;
    }

    pid = clone(child_fn,
                (char *)stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0) {
        free(stack);
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "clone failed: %s", strerror(errno));
        return -1;
    }

    close(pipefd[1]);
    free(cfg);
    free(stack);

    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "calloc record failed");
        return -1;
    }

    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    strncpy(rec->rootfs, req->rootfs, sizeof(rec->rootfs) - 1);
    strncpy(rec->command, req->command, sizeof(rec->command) - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice_value = req->nice_value;
    rec->pipe_read_fd = pipefd[0];
    rec->stop_requested = 0;
    rec->exit_code = 0;
    rec->exit_signal = 0;
    snprintf(rec->final_reason, sizeof(rec->final_reason), "running");
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);

    pthread_mutex_init(&rec->wait_lock, NULL);
    pthread_cond_init(&rec->wait_cv, NULL);

    parg = calloc(1, sizeof(*parg));
    if (!parg) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        pthread_cond_destroy(&rec->wait_cv);
        pthread_mutex_destroy(&rec->wait_lock);
        free(rec);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "calloc producer arg failed");
        return -1;
    }

    parg->ctx = ctx;
    parg->rec = rec;

    if (pthread_create(&rec->producer_thread, NULL, producer_thread_main, parg) != 0) {
        free(parg);
        close(pipefd[0]);
        kill(pid, SIGKILL);
        pthread_cond_destroy(&rec->wait_cv);
        pthread_mutex_destroy(&rec->wait_lock);
        free(rec);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "pthread_create failed");
        return -1;
    }
    rec->producer_started = 1;

    if (register_with_monitor(ctx->monitor_fd,
                              rec->id,
                              rec->host_pid,
                              rec->soft_limit_bytes,
                              rec->hard_limit_bytes) != 0) {
        perror("register_with_monitor");
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (out_rec)
        *out_rec = rec;

    resp->status = 0;
    resp->exit_code = 0;
    snprintf(resp->message, sizeof(resp->message),
             "started container=%s pid=%d", rec->id, rec->host_pid);
    return 0;
}

static void append_line(char *dst, size_t size, const char *line)
{
    size_t used = strlen(dst);
    size_t remaining;

    if (used >= size - 1)
        return;

    remaining = size - used - 1;
    strncat(dst, line, remaining);
}

static int handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    container_record_t *cur;
    char line[512];

    resp->status = 0;
    resp->exit_code = 0;
    resp->message[0] = '\0';

    append_line(resp->message, sizeof(resp->message),
                "ID\tPID\tSTATE\tSOFT(MiB)\tHARD(MiB)\tSTARTED\tREASON\n");

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        char ts[64];
        format_time(cur->started_at, ts, sizeof(ts));
        snprintf(line, sizeof(line),
                 "%s\t%d\t%s\t%lu\t%lu\t%s\t%s\n",
                 cur->id,
                 cur->host_pid,
                 state_to_string(cur->state),
                 cur->soft_limit_bytes >> 20,
                 cur->hard_limit_bytes >> 20,
                 ts,
                 cur->final_reason[0] ? cur->final_reason : "-");
        append_line(resp->message, sizeof(resp->message), line);
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    return 0;
}

static int handle_logs(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       control_response_t *resp)
{
    container_record_t *rec;
    int fd;
    ssize_t n;

    (void)ctx;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container_locked(ctx, req->container_id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "no such container: %s", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    fd = open(rec->log_path, O_RDONLY);
    if (fd < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "cannot open log file: %s", rec->log_path);
        return -1;
    }

    n = read(fd, resp->message, sizeof(resp->message) - 1);
    close(fd);

    if (n < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "read log failed");
        return -1;
    }

    resp->message[n] = '\0';
    resp->status = 0;
    resp->exit_code = 0;
    return 0;
}

static int handle_stop(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       control_response_t *resp)
{
    container_record_t *rec;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container_locked(ctx, req->container_id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "no such container: %s", req->container_id);
        return -1;
    }

    if (rec->state != CONTAINER_RUNNING && rec->state != CONTAINER_STARTING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container is not running: %s", req->container_id);
        return -1;
    }

    rec->stop_requested = 1;
    pthread_mutex_unlock(&ctx->metadata_lock);

    kill(rec->host_pid, SIGTERM);

    resp->status = 0;
    resp->exit_code = 0;
    snprintf(resp->message, sizeof(resp->message),
             "stop requested for %s", req->container_id);
    return 0;
}

static int handle_run_wait(container_record_t *rec,
                           control_response_t *resp)
{
    pthread_mutex_lock(&rec->wait_lock);
    while (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)
        pthread_cond_wait(&rec->wait_cv, &rec->wait_lock);
    pthread_mutex_unlock(&rec->wait_lock);

    resp->status = 0;
    resp->exit_code = rec->exit_code;
    snprintf(resp->message, sizeof(resp->message),
             "container=%s finished state=%s exit_code=%d reason=%s",
             rec->id,
             state_to_string(rec->state),
             rec->exit_code,
             rec->final_reason);
    return 0;
}

static int handle_control_request(supervisor_ctx_t *ctx,
                                  const control_request_t *req,
                                  control_response_t *resp)
{
    switch (req->kind) {
    case CMD_START:
        return start_container(ctx, req, NULL, resp);

    case CMD_RUN: {
        container_record_t *rec = NULL;
        if (start_container(ctx, req, &rec, resp) != 0)
            return -1;
        return handle_run_wait(rec, resp);
    }

    case CMD_PS:
        return handle_ps(ctx, resp);

    case CMD_LOGS:
        return handle_logs(ctx, req, resp);

    case CMD_STOP:
        return handle_stop(ctx, req, resp);

    default:
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "unsupported command");
        return -1;
    }
}

static void handle_sigchld(int signo)
{
    (void)signo;
}

static void handle_shutdown(int signo)
{
    (void)signo;
    if (g_ctx) {
        g_ctx->should_stop = 1;
        if (g_ctx->server_fd >= 0)
            close(g_ctx->server_fd);
    }
}

static int setup_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;

    return 0;
}

static void cleanup_supervisor(supervisor_ctx_t *ctx)
{
    container_record_t *cur, *next;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
            cur->stop_requested = 1;
            kill(cur->host_pid, SIGTERM);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    sleep(1);
    reap_children(ctx);

    bounded_buffer_begin_shutdown(&ctx->log_buffer);
    pthread_join(ctx->logger_thread, NULL);

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        next = cur->next;
        if (cur->producer_started)
            pthread_join(cur->producer_thread, NULL);
        if (cur->pipe_read_fd >= 0)
            close(cur->pipe_read_fd);
        pthread_cond_destroy(&cur->wait_cv);
        pthread_mutex_destroy(&cur->wait_lock);
        free(cur);
        cur = next;
    }
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0)
        close(ctx->monitor_fd);

    if (ctx->server_fd >= 0)
        close(ctx->server_fd);

    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx->log_buffer);
    pthread_mutex_destroy(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ensure_log_dir();

    ctx.monitor_fd = open(MONITOR_DEVICE_PATH, O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open monitor device");

    if (setup_signal_handlers() != 0) {
        perror("sigaction");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        return 1;
    }

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        return 1;
    }

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        cleanup_supervisor(&ctx);
        return 1;
    }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        cleanup_supervisor(&ctx);
        return 1;
    }

    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        cleanup_supervisor(&ctx);
        return 1;
    }

    printf("Supervisor listening on %s\n", CONTROL_PATH);
    fflush(stdout);

    g_ctx = &ctx;

    while (!ctx.should_stop) {
        int client_fd;
        control_request_t req;
        control_response_t resp;
        ssize_t n;

        reap_children(&ctx);

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (ctx.should_stop)
                break;
            perror("accept");
            continue;
        }

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        n = read(client_fd, &req, sizeof(req));
        if (n != (ssize_t)sizeof(req)) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "invalid request");
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            continue;
        }

        handle_control_request(&ctx, &req, &resp);
        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }

    cleanup_supervisor(&ctx);
    g_ctx = NULL;
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    n = read(fd, &resp, sizeof(resp));
    if (n != (ssize_t)sizeof(resp)) {
        perror("read");
        close(fd);
        return 1;
    }

    close(fd);

    if (resp.message[0] != '\0')
        printf("%s\n", resp.message);

    if (req->kind == CMD_RUN)
        return resp.exit_code;

    return (resp.status == 0) ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
