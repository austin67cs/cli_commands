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

uint8_t num_width(size_t value);
int list(const char *path, int flags);
int cmp_name(const void *, const void *);
int get_entries(DIR *dirp, int flags, struct dirent ***entries,
                size_t *entries_count);
int get_stats(DIR *dirp, struct dirent **entries, size_t entries_count,
              struct stat **entries_stats);
int list_dir(const char *path, int flags);
void list_long(struct stat *st, const char *file_name, uint8_t max_col_nlink,
               uint8_t max_col_size);
static char *format_file_mode(mode_t st_mode);
static char *format_time(struct tm *tm);
int list_dir_entries(const char *path, int flags);

struct stats_meta {
  uint8_t link_col_width;
  uint8_t size_col_width;
  uint8_t usr_col_width;
  uint8_t grp_col_width;
};

#define GET_LINK_COL_WIDTH(stats)                                              \
  ((struct stats_meta *)(stats) - 1)->link_col_width

#define GET_SIZE_COL_WIDTH(stats)                                              \
  ((struct stats_meta *)(stats) - 1)->size_col_width

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
  struct dirent *entry_a = *((struct dirent **)a);
  struct dirent *entry_b = *((struct dirent **)b);

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

  list_long(&st, path, 5, 5);
  return 0;
}

int list_dir_entries(const char *path, int flags) {
  DIR *dirp = opendir(path);

  if (dirp == NULL) {
    fprintf(stderr, "%s: opendir: %s\n", path, strerror(errno));
    closedir(dirp);
    return -1;
  }

  struct dirent **entries = NULL;
  size_t entries_count = 0;

  if (get_entries(dirp, flags, &entries, &entries_count) == -1) {
    perror("get_entries");
    return -1;
  }
  qsort(entries, entries_count, sizeof(*entries), cmp_name);

  if (!HAS_FLAG(flags, LS_L)) {
    for (size_t i = 0; i < entries_count; i++) {
      printf("%s ", entries[i]->d_name);
    }
  } else {
    struct stat *entries_stats;
    if (get_stats(dirp, entries, entries_count, &entries_stats) == -1)
      return -1;

    uint8_t nlink_col_width = GET_LINK_COL_WIDTH(entries_stats);
    uint8_t size_col_width = GET_SIZE_COL_WIDTH(entries_stats);

    for (size_t i = 0; i < entries_count; i++) {
      list_long(&entries_stats[i], entries[i]->d_name, nlink_col_width,
                size_col_width);
    }
  }

  closedir(dirp);
  return 0;
}

int get_stats(DIR *dirp, struct dirent **entries, size_t count,
              struct stat **stats) {

  struct stats_meta *meta =
      malloc(sizeof(struct stats_meta) + (sizeof(**stats) * count));

  if (meta == NULL)
    return 1;

  __nlink_t max_nlink = 0;
  __off_t max_size = 0;
  size_t max_usr = 0;
  size_t max_grp = 0;

  struct stat *tmp = (struct stat *)(meta + 1);

  int fd = dirfd(dirp);
  for (size_t i = 0; i < count; i++) {
    if (fstatat(fd, entries[i]->d_name, &tmp[i], 0) == -1) {
      free(meta);
      return -1;
    }

    if (tmp[i].st_nlink > max_nlink)
      max_nlink = tmp[i].st_nlink;

    if (tmp[i].st_size > max_size)
      max_size = tmp[i].st_size;
  }

  meta->link_col_width = num_width(max_nlink);
  meta->size_col_width = num_width(max_size);

  *stats = tmp;
  return 0;
}

/**
 * Writes into the entries and entries_count variables the directory entries
 * retrieved. Does not modify entries or entries_count on Failure. Returns -1 on
 * failure and 0 on success.
 */
int get_entries(DIR *dirp, int flags, struct dirent ***entries,
                size_t *entries_count) {
  struct dirent *entry = NULL;
  struct dirent **local_entries = NULL;
  size_t local_entries_count = 0;

  while ((entry = readdir(dirp)) != NULL) {
    if (!HAS_FLAG(flags, LS_A) && entry->d_name[0] == '.')
      continue;

    // Resize the list of entries to accomodate new entry.
    void *tmp_local_entries = realloc(
        local_entries, (local_entries_count + 1) * sizeof(*local_entries));

    if (tmp_local_entries == NULL)
      goto clean_local_entries;

    local_entries = tmp_local_entries;

    // Local pointer variable to hold a copy of the entry.
    void *local_entry = malloc(entry->d_reclen);

    if (local_entry == NULL)
      goto clean_local_entries;

    local_entries[local_entries_count] = local_entry;
    memcpy(local_entries[local_entries_count], entry, entry->d_reclen);
    local_entries_count++;
  }

  *entries = local_entries;
  *entries_count = local_entries_count;
  return 0;

clean_local_entries:
  for (size_t i = 0; i < local_entries_count; i++) {
    free(local_entries[i]);
  }
  free(local_entries);
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

void list_long(struct stat *st, const char *file_name, uint8_t nlink_col_width,
               uint8_t size_col_width) {

  printf("%s %*lu %s %s %*ld %s %s \n", format_file_mode(st->st_mode),
         nlink_col_width, st->st_nlink, getpwuid(st->st_uid)->pw_name,
         getgrgid(st->st_gid)->gr_name, size_col_width, st->st_size,
         format_time(localtime(&st->st_mtim.tv_sec)), file_name);
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
