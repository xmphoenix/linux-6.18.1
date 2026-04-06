// SPDX-License-Identifier: GPL-2.0
/*
 * fanotify file/directory change monitor
 *
 * Usage:
 *   ./fanotify_monitor <path> [path2] [path3] ...
 *
 * Examples:
 *   ./fanotify_monitor /tmp
 *   ./fanotify_monitor /home/user/docs /var/log
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/inotify.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

static const char *event_to_str(uint64_t mask)
{
	/* Return a human-readable string for the event type */
	if (mask & FAN_CREATE)        return "CREATE";
	if (mask & FAN_DELETE)        return "DELETE";
	if (mask & FAN_MOVED_FROM)    return "MOVED_FROM";
	if (mask & FAN_MOVED_TO)      return "MOVED_TO";
	if (mask & FAN_MODIFY)        return "MODIFY";
	if (mask & FAN_CLOSE_WRITE)   return "CLOSE_WRITE";
	if (mask & FAN_CLOSE_NOWRITE) return "CLOSE_NOWRITE";
	if (mask & FAN_OPEN)          return "OPEN";
	if (mask & FAN_ATTRIB)        return "ATTRIB";
	if (mask & FAN_ACCESS)        return "ACCESS";
	if (mask & FAN_DELETE_SELF)   return "DELETE_SELF";
	if (mask & FAN_MOVE_SELF)     return "MOVE_SELF";
	if (mask & FAN_ONDIR)         return "ONDIR";
	return "UNKNOWN";
}

/* Read /proc/self/fd/<fd> to get the file path */
static void get_path_from_fd(int fd, char *buf, size_t bufsize)
{
	char proc_path[64];
	ssize_t len;

	snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
	len = readlink(proc_path, buf, bufsize - 1);
	if (len > 0)
		buf[len] = '\0';
	else
		snprintf(buf, bufsize, "<unknown>");
}

/* Read /proc/<pid>/cmdline to get the command that triggered the event */
static void get_process_cmdline(pid_t pid, char *buf, size_t bufsize)
{
	char proc_path[64];
	int fd;
	ssize_t len;
	size_t i;

	snprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline", pid);
	fd = open(proc_path, O_RDONLY);
	if (fd < 0) {
		snprintf(buf, bufsize, "<pid %d exited>", pid);
		return;
	}

	len = read(fd, buf, bufsize - 1);
	close(fd);

	if (len <= 0) {
		snprintf(buf, bufsize, "<pid %d>", pid);
		return;
	}

	buf[len] = '\0';

	/* cmdline args are separated by '\0', replace with spaces */
	for (i = 0; i < (size_t)len - 1; i++) {
		if (buf[i] == '\0')
			buf[i] = ' ';
	}
}

/* Read /proc/<pid>/exe to get the executable path */
static void get_process_exe(pid_t pid, char *buf, size_t bufsize)
{
	char proc_path[64];
	ssize_t len;

	snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", pid);
	len = readlink(proc_path, buf, bufsize - 1);
	if (len > 0)
		buf[len] = '\0';
	else
		snprintf(buf, bufsize, "<unknown>");
}

static const char *inotify_event_to_str(uint32_t mask)
{
	if (mask & IN_CREATE)      return "CREATE";
	if (mask & IN_DELETE)      return "DELETE";
	if (mask & IN_MOVED_FROM)  return "MOVED_FROM";
	if (mask & IN_MOVED_TO)    return "MOVED_TO";
	if (mask & IN_MODIFY)      return "MODIFY";
	if (mask & IN_CLOSE_WRITE) return "CLOSE_WRITE";
	if (mask & IN_ATTRIB)      return "ATTRIB";
	if (mask & IN_DELETE_SELF) return "DELETE_SELF";
	if (mask & IN_MOVE_SELF)   return "MOVE_SELF";
	return "UNKNOWN";
}

static void handle_inotify_events(int inotify_fd, int argc, char *argv[])
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	const struct inotify_event *event;
	char *ptr;

	len = read(inotify_fd, buf, sizeof(buf));
	if (len <= 0) {
		if (len < 0 && errno != EAGAIN)
			perror("read");
		return;
	}

	for (ptr = buf; ptr < buf + len;
	     ptr += sizeof(struct inotify_event) + event->len) {
		event = (const struct inotify_event *)ptr;

		/* Find the watched directory path from wd */
		const char *dir = "?";
		int w;
		for (w = 1; w < argc; w++) {
			if (event->wd == w)
				dir = argv[w];
		}

		printf("[%-12s] %s%s/%s\n",
		       inotify_event_to_str(event->mask),
		       (event->mask & IN_ISDIR) ? "(dir) " : "",
		       dir,
		       (event->len > 0) ? event->name : "");
	}
}

static void handle_events(int fanotify_fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
	ssize_t len;
	const struct fanotify_event_metadata *meta;

	len = read(fanotify_fd, buf, sizeof(buf));
	if (len <= 0) {
		if (len < 0 && errno != EAGAIN)
			perror("read");
		return;
	}

	for (meta = (struct fanotify_event_metadata *)buf;
	     FAN_EVENT_OK(meta, len);
	     meta = FAN_EVENT_NEXT(meta, len)) {

		char cmdline[1024] = "";
		char exe[PATH_MAX] = "";

		if (meta->vers != FANOTIFY_METADATA_VERSION) {
			fprintf(stderr, "Mismatch fanotify metadata version\n");
			return;
		}

		/* Get process info for the triggering PID */
		if (meta->pid > 0) {
			get_process_cmdline(meta->pid, cmdline, sizeof(cmdline));
			get_process_exe(meta->pid, exe, sizeof(exe));
		}

		/* Check for FAN_EVENT_INFO_TYPE events (FID-based) */
		if (meta->fd == FAN_NOFD) {
			/*
			 * For FAN_REPORT_FID / FAN_REPORT_DFID_NAME mode,
			 * extract info from event info records.
			 */
			struct fanotify_event_info_fid *fid;
			struct fanotify_event_info_header *info;
			const char *file_name = NULL;
			char *info_ptr = (char *)(meta + 1);
			int remaining = meta->event_len - sizeof(*meta);

			while (remaining >= (int)sizeof(struct fanotify_event_info_header)) {
				info = (struct fanotify_event_info_header *)info_ptr;

				if (info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
					fid = (struct fanotify_event_info_fid *)info;
					/* file_name follows the file_handle */
					struct file_handle *fh = (struct file_handle *)fid->handle;
					file_name = (const char *)(fh->f_handle + fh->handle_bytes);
					if (*file_name == '\0')
						file_name = NULL;
				}

				info_ptr += info->len;
				remaining -= info->len;
			}

			printf("[%-12s] pid=%-6d %s%s\n"
			       "              exe: %s\n"
			       "              cmd: %s\n",
			       event_to_str(meta->mask),
			       meta->pid,
			       (meta->mask & FAN_ONDIR) ? "(dir) " : "",
			       file_name ? file_name : "<dir-event>",
			       exe, cmdline);
		} else {
			/* fd-based event */
			char path[PATH_MAX];
			get_path_from_fd(meta->fd, path, sizeof(path));

			printf("[%-12s] pid=%-6d %s\n"
			       "              exe: %s\n"
			       "              cmd: %s\n",
			       event_to_str(meta->mask),
			       meta->pid,
			       path,
			       exe, cmdline);

			close(meta->fd);
		}
	}
}

int main(int argc, char *argv[])
{
	int fanotify_fd;
	struct pollfd pfd;
	int i;
	int use_fid = 1; /* try FID mode first, fallback to fd mode */

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <path> [path2] ...\n", argv[0]);
		return 1;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	/*
	 * Try FID mode first (FAN_REPORT_DFID_NAME + FAN_MARK_FILESYSTEM).
	 * This gives filenames in events but requires filesystem support for
	 * name_to_handle_at(). Overlayfs, tmpfs, etc. may not support it.
	 * If it fails, fall back to fd-based mode (FAN_MARK_MOUNT).
	 */
	fanotify_fd = fanotify_init(
		FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_NONBLOCK,
		O_RDONLY | O_LARGEFILE);
	if (fanotify_fd < 0) {
		perror("fanotify_init");
		fprintf(stderr, "Note: requires CAP_SYS_ADMIN or root\n");
		return 1;
	}

	/* First pass: try FID mode with FAN_MARK_FILESYSTEM */
	for (i = 1; i < argc; i++) {
		int ret = fanotify_mark(fanotify_fd,
			FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
			FAN_CREATE | FAN_DELETE | FAN_MODIFY |
			FAN_MOVED_FROM | FAN_MOVED_TO |
			FAN_ATTRIB | FAN_CLOSE_WRITE |
			FAN_ONDIR | FAN_EVENT_ON_CHILD,
			AT_FDCWD, argv[i]);
		if (ret < 0) {
			fprintf(stderr,
				"FID mode unsupported for %s (%s), "
				"falling back to fd mode...\n",
				argv[i], strerror(errno));
			use_fid = 0;
			break;
		}
	}

	/*
	 * Fallback: use inotify instead of fanotify.
	 * inotify works on overlayfs and supports all event types
	 * (CREATE, DELETE, MODIFY, MOVE, etc.).
	 * Note: inotify does not report the triggering PID.
	 */
	if (!use_fid) {
		int inotify_fd;

		close(fanotify_fd);
		fanotify_fd = -1;

		inotify_fd = inotify_init1(IN_NONBLOCK);
		if (inotify_fd < 0) {
			perror("inotify_init1");
			return 1;
		}

		for (i = 1; i < argc; i++) {
			int wd = inotify_add_watch(inotify_fd, argv[i],
				IN_CREATE | IN_DELETE | IN_MODIFY |
				IN_MOVED_FROM | IN_MOVED_TO |
				IN_ATTRIB | IN_CLOSE_WRITE);
			if (wd < 0) {
				fprintf(stderr, "inotify_add_watch(%s): %s\n",
					argv[i], strerror(errno));
				close(inotify_fd);
				return 1;
			}
		}

		for (i = 1; i < argc; i++)
			printf("Monitoring: %s [inotify mode]\n", argv[i]);
		printf("Note: inotify mode does not report PID info.\n");
		printf("Waiting for events (Ctrl+C to stop)...\n\n");

		pfd.fd = inotify_fd;
		pfd.events = POLLIN;

		while (running) {
			int ret = poll(&pfd, 1, 500);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				perror("poll");
				break;
			}
			if (ret > 0 && (pfd.revents & POLLIN))
				handle_inotify_events(inotify_fd, argc, argv);
		}

		printf("\nStopped.\n");
		close(inotify_fd);
		return 0;
	}

	/* FID mode path */
	for (i = 1; i < argc; i++)
		printf("Monitoring: %s [FID mode]\n", argv[i]);
	printf("Waiting for events (Ctrl+C to stop)...\n\n");

	pfd.fd = fanotify_fd;
	pfd.events = POLLIN;

	while (running) {
		int ret = poll(&pfd, 1, 500);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (ret > 0 && (pfd.revents & POLLIN))
			handle_events(fanotify_fd);
	}

	printf("\nStopped.\n");
	close(fanotify_fd);
	return 0;
}
