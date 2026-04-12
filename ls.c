#include "./dynamic_array.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Flags
#define LS_A (1 << 0) // list all.
#define LS_L (1 << 1) // list long.
#define LS_D (1 << 2) // list self.
#define HAS_FLAG(flags, f) ((flags) & (f))

/**
 * Computes the number of digits in a number.
 */
uint8_t num_width(size_t value);

int list(const char *path, int flags);
int cmp_name(const void *, const void *);
int list_dir(const char *path, int flags);
static char *format_file_mode(mode_t st_mode);
static char *format_time(struct tm *tm);
int list_dir_entries(const char *path, int flags);

struct stats_meta {
  uint8_t link_col_width;
  uint8_t size_col_width;
  uint8_t usr_col_width;
  uint8_t grp_col_width;
  void *usr_grp_names;
};

struct id_name_pair {
  uint32_t id;
  char *name;
};

struct d_entry {
  ino_t d_ino;
  char *d_name;
};

void list_long(struct d_entry *entries, struct stat *stats);
int get_stats(DIR *dirp, struct d_entry *entries, struct stat **entries_stats);
int get_entries(DIR *dirp, int flags, struct d_entry **entries);
void free_entries(struct d_entry *entries);

static ssize_t get_usr_name_cache_idx(struct id_name_pair *cache, uint32_t id) {
  // Stores the most recently accessed cached file user id against its index in
  // the cache array.
  static struct {
    uint32_t id;
    ssize_t idx;
  } last = {.idx = -1};

  if (last.idx != -1 && last.id == id)
    return last.idx;

  size_t length = ARR_LENGTH(cache);
  for (size_t i = 0; i < length; i++) {
    if (cache[i].id == id) {
      last.id = id;
      last.idx = i;
      return i;
    }
  }
  return -1;
}

static ssize_t get_grp_name_cache_idx(struct id_name_pair *cache, uint32_t id) {
  // Stores the most recently accessed cached file group id against its index in
  // the cache array.
  static struct {
    uint32_t id;
    ssize_t idx;
  } last = {.idx = -1};

  if (last.idx != -1 && last.id == id)
    return last.idx;

  size_t length = ARR_LENGTH(cache);
  for (size_t i = 0; i < length; i++) {
    if (cache[i].id == id) {
      last.id = id;
      last.idx = i;
      return i;
    }
  }
  return -1;
}

#define STATS_HEADER(stats) ((struct stats_meta *)(stats) - 1)
#define GET_LINK_COL_WIDTH(stats) (STATS_HEADER(stats)->link_col_width)
#define GET_SIZE_COL_WIDTH(stats) (STATS_HEADER(stats)->size_col_width)
#define GET_MAX_USR_NAME_LEN(stats) (STATS_HEADER(stats)->usr_col_width)
#define GET_MAX_GRP_NAME_LEN(stats) (STATS_HEADER(stats)->grp_col_width)
#define GET_USR_GRP_NAMES(stats) (STATS_HEADER(stats)->usr_grp_names)
#define SET_MAX_USR_NAME_LEN(stats, n)                                         \
  (STATS_HEADER(stats)->usr_col_width = (n))
#define SET_MAX_GRP_NAME_LEN(stats, n)                                         \
  (STATS_HEADER(stats)->grp_col_width = (n))

static inline void free_usr_grp_names_cache(struct id_name_pair *cache) {
  size_t length = ARR_LENGTH(cache);
  for (size_t i = 0; i < length; i++)
    free(cache[i].name);
  ARR_FREE(cache);
}

int main(int argc, char *argv[]) {

  int flags = 0;
  int opt;

  while ((opt = getopt(argc, argv, "ald")) != -1) {
    switch (opt) {
    case 'a':
      flags |= LS_A;
      break;
    case 'l':
      flags |= LS_L;
      break;
    case 'd':
      flags |= LS_D;
      break;
    case ':':
      printf("%s: invalid option -- '%c\n'", argv[0], optopt);
      exit(EXIT_FAILURE);
    }
  }

  bool print_title = argc - optind > 1 && !HAS_FLAG(flags, LS_D);

  if (optind == argc) {
    // Default to current working directory.
    if (list(".", flags) == -1)
      exit(EXIT_FAILURE);
  } else {
    while (optind < argc) {
      if (print_title)
        printf("%s:\n", argv[optind]);
      if (list(argv[optind], flags) == -1)
        exit(EXIT_FAILURE);
      optind++;
    }
  }
}

/**
 * Handles listing of directries or directory contents.
 * Return -1 on failure or 0 otherwise.
 */
int list(const char *path, int flags) {
  // List a given directory if option 'd' is passed.
  if (HAS_FLAG(flags, LS_D)) {
    return list_dir(path, flags);
  }

  return list_dir_entries(path, flags);
}

int cmp_name(const void *a, const void *b) {
  struct d_entry *entry_a = (struct d_entry *)a;
  struct d_entry *entry_b = (struct d_entry *)b;

  return strcmp(entry_a->d_name, entry_b->d_name);
}

/**
 * Prints a given path or its long format if -l option is passed.
 * Returns 0 on success and -1 otherwise.
 * -1 is returned if for some reason the 'stat' of a given path could not be
 * retrieved.
 */
int list_dir(const char *path, int flags) {
  if (!HAS_FLAG(flags, LS_L)) {
    printf("%s", path);
    return 0;
  }

  struct stat st;
  if (stat(path, &st) == -1) {
    return -1;
  }

  /* list_long(&st, path, 5, 5); */
  return 0;
}

int list_dir_entries(const char *path, int flags) {
  DIR *dirp = opendir(path);

  if (dirp == NULL) {
    fprintf(stderr, "%s: opendir: %s\n", path, strerror(errno));
    closedir(dirp);
    return -1;
  }

  struct d_entry *entries = NULL;

  if (get_entries(dirp, flags, &entries) == -1) {
    perror("get_entries");
    closedir(dirp);
    return -1;
  }
  size_t entries_count = ARR_LENGTH(entries);
  qsort(entries, entries_count, sizeof(*entries), cmp_name);

  if (!HAS_FLAG(flags, LS_L)) {
    for (size_t i = 0; i < entries_count; i++) {
      printf("%s ", entries[i].d_name);
    }
  } else {
    struct stat *entries_stats;
    if (get_stats(dirp, entries, &entries_stats) == -1) {
      closedir(dirp);
      return -1;
    }

    uint8_t nlink_col_width = GET_LINK_COL_WIDTH(entries_stats);
    uint8_t size_col_width = GET_SIZE_COL_WIDTH(entries_stats);

    list_long(entries, entries_stats);
    struct stats_meta *stats_meta = STATS_HEADER(entries_stats);

    free_usr_grp_names_cache(stats_meta->usr_grp_names);

    // Free stats from its meta.
    free(stats_meta);
  }

  free_entries(entries);
  closedir(dirp);
  return 0;
}

int get_stats(DIR *dirp, struct d_entry *entries, struct stat **stats) {

  size_t entries_count = ARR_LENGTH(entries);

  // Memory allocation to hold the file stats retrieved and its metadata.
  struct stats_meta *stats_meta =
      malloc(sizeof(struct stats_meta) + (sizeof(**stats) * entries_count));
  if (stats_meta == NULL)
    return 1;

  // Actual memory where retrieved stats live.
  struct stat *local_stats = (struct stat *)(stats_meta + 1);

  // Memory allocation to cache file owner name and file group name to reduce
  // system call.
  struct arr_meta *usr_grp_names_cache_meta =
      malloc(sizeof(struct arr_meta) +
             (sizeof(struct id_name_pair) * ARR_INIT_CAPACITY));
  if (usr_grp_names_cache_meta == NULL)
    return -1;

  // Actual allocation where cache values live.
  struct id_name_pair *usr_grp_names_cache =
      (struct id_name_pair *)(usr_grp_names_cache_meta + 1);

  // Initialization of metadata.
  ARR_SET_CAPACITY(usr_grp_names_cache, ARR_INIT_CAPACITY);
  ARR_SET_LENGTH(usr_grp_names_cache, 0);

  __nlink_t max_nlink = 0;
  __off_t max_size = 0;

  // Initialization of metadata.
  SET_MAX_USR_NAME_LEN(local_stats, 0);
  SET_MAX_GRP_NAME_LEN(local_stats, 0);

  int fd = dirfd(dirp);
  for (size_t i = 0; i < entries_count; i++) {
    int8_t state = fstatat(fd, entries[i].d_name, &local_stats[i], 0);

    if (state == -1) {
      free(stats_meta);
      return -1;
    }

    size_t cache_length = ARR_LENGTH(usr_grp_names_cache);

    if (cache_length == 0 ||
        get_usr_name_cache_idx(usr_grp_names_cache, local_stats[i].st_uid) ==
            -1) {

      struct passwd *passwd = getpwuid(local_stats[i].st_uid);
      if (passwd == NULL)
        return -1;

      char *file_usr_name = passwd->pw_name;
      if (cache_length == ARR_CAPACITY(usr_grp_names_cache)) {

        size_t new_capacity = ARR_NEW_CAPACITY(usr_grp_names_cache);
        void *state = ARR_RESIZE(usr_grp_names_cache, new_capacity);

        if (state == NULL)
          // TODO: Try implementing a fall through such that if caching fails
          // program still continues and retrieve file user name and file group
          // name one file at a time. But this would mean potential wasteful
          // system calls.
          return -1;
      }

      usr_grp_names_cache[cache_length].id = local_stats[i].st_uid;
      usr_grp_names_cache[cache_length].name = strdup(file_usr_name);

      cache_length++;
      ARR_SET_LENGTH(usr_grp_names_cache, cache_length);

      size_t file_usr_name_len = strlen(file_usr_name);

      if (file_usr_name_len > GET_MAX_USR_NAME_LEN(local_stats))
        SET_MAX_USR_NAME_LEN(local_stats, file_usr_name_len);
    }

    if (cache_length == 0 ||
        get_grp_name_cache_idx(usr_grp_names_cache, local_stats[i].st_gid) ==
            -1) {

      struct group *group = getgrgid(local_stats[i].st_gid);
      if (group == NULL)
        return -1;

      char *file_grp_name = group->gr_name;
      if (cache_length == ARR_CAPACITY(usr_grp_names_cache)) {

        size_t new_capacity = ARR_NEW_CAPACITY(usr_grp_names_cache);
        void *status = ARR_RESIZE(usr_grp_names_cache, new_capacity);

        if (status == NULL)
          // TODO: Try implementing a fall through such that if caching fails
          // program still continues and retrieve file user name and file group
          // name one file at a time. But this would mean potential wasteful
          // system calls.
          return -1;
      }

      usr_grp_names_cache[cache_length].id = local_stats[i].st_gid;
      usr_grp_names_cache[cache_length].name = strdup(file_grp_name);

      cache_length++;
      ARR_SET_LENGTH(usr_grp_names_cache, cache_length);

      size_t file_grp_name_len = strlen(file_grp_name);

      if (file_grp_name_len > GET_MAX_GRP_NAME_LEN(local_stats))
        SET_MAX_GRP_NAME_LEN(local_stats, file_grp_name_len);
    }

    if (local_stats[i].st_nlink > max_nlink)
      max_nlink = local_stats[i].st_nlink;

    if (local_stats[i].st_size > max_size)
      max_size = local_stats[i].st_size;
  }

  // Check if maximum group name length is zero in which the file user name and
  // the file group name are the same for each file.
  if (GET_MAX_GRP_NAME_LEN(local_stats) == 0)
    SET_MAX_GRP_NAME_LEN(local_stats, GET_MAX_USR_NAME_LEN(local_stats));

  stats_meta->link_col_width = num_width(max_nlink);
  stats_meta->size_col_width = num_width(max_size);
  stats_meta->usr_grp_names = usr_grp_names_cache;

  *stats = local_stats;
  return 0;
}

int get_entries(DIR *dirp, int flags, struct d_entry **entries) {

  struct dirent *entry = NULL;
  struct d_entry *lentries = NULL;

  while ((entry = readdir(dirp)) != NULL) {
    if (!HAS_FLAG(flags, LS_A) && entry->d_name[0] == '.')
      continue;

    size_t length = ARR_LENGTH(lentries);

    if (length == ARR_CAPACITY(lentries)) {
      size_t new_capacity = ARR_NEW_CAPACITY(lentries);

      struct arr_meta *meta = ARR_RESIZE(lentries, new_capacity);
      if (meta == NULL)
        goto clean_lentries;

      meta->capacity = new_capacity;
      lentries = (struct d_entry *)(meta + 1);
    }

    memcpy(&lentries[length].d_ino, &entry->d_ino, sizeof(entry->d_ino));
    lentries[length].d_name = strdup(entry->d_name);

    ARR_SET_LENGTH(lentries, length + 1);
  }

  *entries = lentries;
  return 0;

clean_lentries:
  free_entries(lentries);
  return -1;
}

/**
 * Computes the number of digits of a number.
 */
uint8_t num_width(size_t value) {
  uint8_t width = 1;
  while (value > 10) {
    value /= 10;
    width++;
  }
  return width;
}

void list_long(struct d_entry *entries, struct stat *stats) {

  /* struct stat *st, const char *file_name, uint8_t nlink_col_width,
                 uint8_t size_col_width */

  size_t length = ARR_LENGTH(entries);
  struct id_name_pair *usr_grp_names = GET_USR_GRP_NAMES(stats);

  for (size_t i = 0; i < length; i++) {

    struct stat st = stats[i];

    char *usr_name =
        usr_grp_names[get_usr_name_cache_idx(usr_grp_names, st.st_uid)].name;
    char *grp_name =
        usr_grp_names[get_grp_name_cache_idx(usr_grp_names, st.st_gid)].name;

    printf("%s %*lu %*s %-*s %*ld %s %s \n", format_file_mode(st.st_mode),
           GET_LINK_COL_WIDTH(stats), st.st_nlink, GET_MAX_USR_NAME_LEN(stats),
           usr_name, GET_MAX_GRP_NAME_LEN(stats), grp_name,
           GET_SIZE_COL_WIDTH(stats), st.st_size,
           format_time(localtime(&st.st_mtim.tv_sec)), entries[i].d_name);
  }
}

static char *format_time(struct tm *tm) {
  static char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static char mtime_s[15];

  snprintf(mtime_s, sizeof(mtime_s), "%s %2i %02i:%02i", months[tm->tm_mon],
           tm->tm_mday, tm->tm_hour, tm->tm_min);
  return mtime_s;
}

static char *format_file_mode(mode_t st_mode) {
  static char mode[] = "----------";
  const char chars[] = {'r', 'w', 'x'};

  mode[0] = (S_IFDIR & st_mode) ? 'd' : '-';

  const mode_t masks[] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                          S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};

  for (int i = 0; i < sizeof(masks) / sizeof(int); i++) {
    mode[1 + i] = (masks[i] & st_mode) ? chars[i % sizeof(chars)] : '-';
  }

  return mode;
}

inline void free_entries(struct d_entry *entries) {
  size_t count = ARR_LENGTH(entries);
  for (size_t i = 0; i < count; i++)
    free(entries[i].d_name);

  ARR_FREE(entries);
}
