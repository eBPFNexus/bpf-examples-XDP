#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h> // For if_nametoindex
//#include <linux/err.h> // For IS_ERR_OR_NULL macro // use libbpf_get_error instead
#include <arpa/inet.h> // For inet_ntoa and ntohs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h> // For detecting Ctrl-C
#include <sys/resource.h> // For setting rlmit
#include <time.h>
#include <pthread.h>
#include "pping.h" //key and value structs for the ts_start map

#define BILLION 1000000000UL
#define MILLION 1000000UL
#define TCBPF_LOADER_SCRIPT "./bpf_egress_loader.sh"
#define PINNED_DIR "/sys/fs/bpf/tc/globals"
#define PPING_XDP_OBJ "pping_kern_xdp.o"
#define XDP_PROG_SEC "pping_ingress"
#define PPING_TCBPF_OBJ "pping_kern_tc.o"
#define TCBPF_PROG_SEC "pping_egress"
#define XDP_FLAGS XDP_FLAGS_UPDATE_IF_NOEXIST
#define MAP_NAME "ts_start"
#define MAP_CLEANUP_INTERVAL 1*BILLION // Clean timestamp map once per second 
#define PERF_BUFFER_NAME "rtt_events"
#define PERF_BUFFER_PAGES 64 // Related to the perf-buffer size?
#define PERF_POLL_TIMEOUT_MS 100
#define RMEMLIM 512UL << 20 /* 512 MBs */
#define MAX_COMMAND_LEN 1024
#define ERROR_MSG_MAX 1024
#define MAX_PATH_LEN 1024
#define TIMESTAMP_LIFETIME 10*BILLION // Clear out entries from ts_start if they're over 10 seconds

/* BPF implementation of pping using libbpf
 * Uses TC-BPF for egress and XDP for ingress
 * On egrees, packets are parsed for TCP TSval, if found added to hashmap using flow+TSval as key, and current time as value
 * On ingress, packets are parsed for TCP TSecr, if found looksup hashmap using reverse-flow+TSecr as key, and calculates RTT as different between now map value
 * Calculated RTTs are pushed to userspace (together with the related flow) and printed out 
 *
 * TODOs:
 *  - Cleanup: Unload TC-BPF at program shutdown, and unpin and delete map - In userspace part
 *  - Add IPv6 support - In TC-BPF, XDP and userspace part
 *  - Check for existance of reverse flow before adding to hash-map (to avoid adding timestamps for flows that we can't see the reverse traffic for) - In TC-BPF part
 *     - This could miss the first few packets, and would not be ideal for short flows
 *  - Keep track of minimum RTT for each flow (done by Pollere's pping, and helps identify buffer bloat) - In XDP part
 *  - Add configurable rate-limit for how often each flow can add entries to the map (prevent high-rate flows from quickly filling up the map) - In TCP-BPF part
 *  - Improve map cleaning: Use a dynamic time to live for hash map entries based on flow's RTT, instead of static 10s limit - In TC-BPF, XDP and userspace
 */


struct map_cleanup_args {
  int map_fd;
  __u64 max_age_ns;
};

static volatile int keep_running = 1;

void abort_program(int sig)
{
  keep_running = 0;
}

static int set_rlimit(long int lim)
{
  struct rlimit rlim = {
    .rlim_cur = lim, 
    .rlim_max = lim,
  };

  return !setrlimit(RLIMIT_MEMLOCK, &rlim) ? 0 : -errno;
}

static int bpf_obj_open(struct bpf_object **obj, const char *obj_path, enum bpf_prog_type prog_type)
{
  struct bpf_object_open_attr attr = {
    .prog_type = prog_type,
    .file = obj_path,
  };
  *obj = bpf_object__open_xattr(&attr);
  return libbpf_get_error(*obj);
}

static int bpf_obj_load(struct bpf_object *obj, enum bpf_prog_type prog_type)
{
  struct bpf_program *prog;
  bpf_object__for_each_program(prog, obj) {
    bpf_program__set_type(prog, prog_type);
  }

  return bpf_object__load(obj);
}

static int reuse_pinned_map(int *map_fd, const char *map_name, const char *pinned_dir, struct bpf_object *obj, struct bpf_map_info *expec_map_info)
{
  struct bpf_map *map;
  struct bpf_map_info map_info = {0};
  __u32 info_len = sizeof(map_info);
  char pinned_map_path[MAX_PATH_LEN];
  int err;

  // Find map in object file
  map = bpf_object__find_map_by_name(obj, map_name);
  err = libbpf_get_error(map);
  if (err) {
    fprintf(stderr, "Could not find map %s in object\n", map_name);
    return err;
  }

  // Find pinned map
  snprintf(pinned_map_path, sizeof(pinned_map_path), "%s/%s", pinned_dir, map_name);
  *map_fd = bpf_obj_get(pinned_map_path);
  if (*map_fd < 0) {
    fprintf(stderr, "Could not find map %s in path %s\n", map_name, pinned_dir);
    return *map_fd;
  }

  // Verify map has expected format
  err = bpf_obj_get_info_by_fd(*map_fd, &map_info, &info_len);
  if (err) {
    fprintf(stderr, "Could not get map info from %s\n", pinned_map_path);
    return err;
  }

  if (map_info.type != expec_map_info->type ||
      map_info.key_size != expec_map_info->key_size ||
      map_info.value_size != expec_map_info->value_size ||
      map_info.max_entries != expec_map_info->max_entries) {
    fprintf(stderr, "Pinned map at %s does not match expected format\n", pinned_map_path);
    return -EINVAL;
  }
  
  // Try reusing map
  err = bpf_map__reuse_fd(map, *map_fd);
  if (err) {
    fprintf(stderr, "Failed reusing map fd\n");
    return err;
  }
  return 0;
}

static int xdp_detach(int ifindex, __u32 xdp_flags) {
  return bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);
}

static int xdp_attach(struct bpf_object *obj, const char *sec, int ifindex, __u32 xdp_flags, bool force)
{
  struct bpf_program *prog;
  int prog_fd;
  int err;

  if (sec)
    prog = bpf_object__find_program_by_title(obj, sec);
  else
    prog = bpf_program__next(NULL, obj);
  prog_fd = bpf_program__fd(prog);
  if (prog_fd < 0) {
    fprintf(stderr, "Could not find program to attach\n");
    return prog_fd;
  }

  if (force) // detach current (if any) xdp-program first
    xdp_detach(ifindex, xdp_flags);
  err = bpf_set_link_xdp_fd(ifindex, prog_fd, xdp_flags);
  if (err < 0) {
    fprintf(stderr, "Failed loading xdp-program on interface %d\n", ifindex);
    return err;
  }
  return 0;
}

static __u64 get_time_ns(clockid_t clockid)
{
  struct timespec t;
  if (clock_gettime(clockid, &t) != 0) // CLOCK_BOOTTIME if using bpf_get_ktime_boot_ns
    return 0;
  return (__u64)t.tv_sec * BILLION + (__u64)t.tv_nsec;
}

static int remove_old_entries_from_map(int map_fd, __u64 max_age)
{
  int removed = 0, entries = 0;
  struct ts_key key, prev_key = {0};
  struct ts_timestamp value;
  bool delete_prev = false;
  __u64 now_nsec = get_time_ns(CLOCK_MONOTONIC);
  if (now_nsec == 0)
    return -errno;

  // Cannot delete current key because then loop will reset, see https://www.bouncybouncy.net/blog/bpf_map_get_next_key-pitfalls/
  while(bpf_map_get_next_key(map_fd, &prev_key, &key) == 0) {
    if (delete_prev) {
      bpf_map_delete_elem(map_fd, &prev_key);
      removed++;
      delete_prev = false;
    }

    if (bpf_map_lookup_elem(map_fd, &key, &value) == 0) {
      if (now_nsec > value.timestamp && now_nsec - value.timestamp > max_age) {
	delete_prev = true;
      }
    }
    entries++;
    prev_key = key;
  }
  if (delete_prev) {
    bpf_map_delete_elem(map_fd, &prev_key);
    removed++;
  }
  __u64 duration = get_time_ns(CLOCK_MONOTONIC) - now_nsec;
  printf("Gone through %d entries and removed %d of them in %llu.%09llu s\n", entries, removed, duration / BILLION, duration % BILLION);
  return removed;
}

static void *periodic_map_cleanup(void *args)
{
  struct map_cleanup_args *argp = args;
  struct timespec interval;
  interval.tv_sec = MAP_CLEANUP_INTERVAL / BILLION;
  interval.tv_nsec = MAP_CLEANUP_INTERVAL % BILLION;
  while (keep_running) {
    remove_old_entries_from_map(argp->map_fd, argp->max_age_ns);
    nanosleep(&interval, NULL);
  }
  pthread_exit(NULL);
}

static void handle_rtt_event(void *ctx, int cpu, void *data, __u32 data_size)
{
  const struct rtt_event *e = data;
  struct in_addr saddr, daddr;
  saddr.s_addr = e->flow.saddr;
  daddr.s_addr = e->flow.daddr;
  printf("%llu.%06llu ms %s:%d+%s:%d\n", e->rtt / MILLION, e->rtt % MILLION,
	 inet_ntoa(saddr), ntohs(e->flow.sport),
	 inet_ntoa(daddr), ntohs(e->flow.dport));	 
}

static void handle_missed_rtt_event(void *ctx, int cpu, __u64 lost_cnt)
{
  fprintf(stderr, "Lost %llu RTT events on CPU %d\n", lost_cnt, cpu);
}

int main(int argc, char *argv[])
{
  if (argc < 2) {
    printf("Usage: ./pping_user <dev>\n");
    return EXIT_FAILURE;
  }

  int err = 0, ifindex = 0;
  bool xdp_attached = false;
   struct perf_buffer *pb = NULL;

   // Increase rlimit
  err = set_rlimit(RMEMLIM);
  if (err) {
    fprintf(stderr, "Could not set rlimit to %ld bytes: %s\n", RMEMLIM, strerror(-err));
    goto cleanup;
  }

  // Get index of interface
  ifindex = if_nametoindex(argv[1]);
  if (ifindex == 0) {
    err = -errno;
    fprintf(stderr, "Could not get index of interface %s: %s\n", argv[1], strerror(-err));
    goto cleanup;
  }

  //Load tc-bpf section on interface egress
  char tc_bpf_load[MAX_COMMAND_LEN];
  snprintf(tc_bpf_load, MAX_COMMAND_LEN, "%s --dev %s --obj %s --sec %s",
	   TCBPF_LOADER_SCRIPT, argv[1], PPING_TCBPF_OBJ, TCBPF_PROG_SEC);
  err = system(tc_bpf_load);
  if (err) {
    fprintf(stderr, "Could not load section %s of %s on interface %s: %s\n",
	    TCBPF_PROG_SEC, PPING_TCBPF_OBJ, argv[1], strerror(err));
    goto cleanup;
  }
  
  // Reuse map pinned by tc for the xpd-program
  struct bpf_object *obj;
  int map_fd = 0;
  struct bpf_map_info expected_map_info = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(struct ts_key),
    .value_size = sizeof(struct ts_timestamp),
    .max_entries = 16384,
  };

  err = bpf_obj_open(&obj, PPING_XDP_OBJ, BPF_PROG_TYPE_XDP);
  if (err) {
    fprintf(stderr, "Failed opening object file %s: %s\n", PPING_XDP_OBJ, strerror(-err));
    goto cleanup;
  }

  err = reuse_pinned_map(&map_fd, MAP_NAME, PINNED_DIR, obj, &expected_map_info);
  if (err) {
    fprintf(stderr, "Failed reusing fd for map %s: %s\n", MAP_NAME, strerror(-err));
    goto cleanup;
  }

  // Load and attach XDP program
  err = bpf_obj_load(obj, BPF_PROG_TYPE_XDP);
  if (err) {
    fprintf(stderr, "Failed loading XDP program: %s\n", strerror(-err));
    goto cleanup;
  }
  err = xdp_attach(obj, XDP_PROG_SEC, ifindex, XDP_FLAGS, false);
  if (err) {
    fprintf(stderr, "Failed attaching XDP program to %s: %s\n", argv[1], strerror(-err));
    goto cleanup;
  }
  xdp_attached = true;

  // Setup periodic cleanup of ts_start
  pthread_t tid;
  struct map_cleanup_args args = {.map_fd = map_fd, .max_age_ns = TIMESTAMP_LIFETIME};
  err = pthread_create(&tid, NULL, periodic_map_cleanup, &args);
  if (err) {
    fprintf(stderr, "Failed starting thread to perform periodic map cleanup: %s\n", strerror(err));
    goto cleanup;
  }

  // Set up perf buffer
  struct perf_buffer_opts pb_opts;
  pb_opts.sample_cb = handle_rtt_event;
  pb_opts.lost_cb = handle_missed_rtt_event;
  
  pb = perf_buffer__new(bpf_object__find_map_fd_by_name(obj, PERF_BUFFER_NAME), PERF_BUFFER_PAGES, &pb_opts);
  err = libbpf_get_error(pb);
  if (err) {
    pb = NULL;
    fprintf(stderr, "Failed to open perf buffer %s: %s\n", PERF_BUFFER_NAME, strerror(err));
    goto cleanup;
  }

  // Clean exit on Ctrl-C
  signal(SIGINT, abort_program);

  // Main loop
  while(keep_running) {
    if ((err = perf_buffer__poll(pb, PERF_POLL_TIMEOUT_MS)) < 0) {
      if (keep_running) // Only print polling error if it wasn't caused by program termination
	fprintf(stderr, "Error polling perf buffer: %s\n", strerror(-err));
      break;
    }
  }
    
 cleanup:
  perf_buffer__free(pb);
  if (xdp_attached) {
    err = xdp_detach(ifindex, XDP_FLAGS);
    if (err) {
      fprintf(stderr, "Failed deatching program from ifindex %d: %s\n", ifindex, strerror(-err));
    }
  }
  // TODO: Unload TC-BPF program
  // TODO: Unpin ts_start map
  
  return err != 0;
}

