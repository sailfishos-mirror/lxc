/* SPDX-License-Identifier: LGPL-2.1+ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define __STDC_FORMAT_MACROS /* Required for PRIu64 to work. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "lsm/lsm.h"
#include "lxclock.h"
#include "memory_utils.h"
#include "namespace.h"
#include "open_utils.h"
#include "parse.h"
#include "process_utils.h"
#include "syscall_wrappers.h"
#include "utils.h"

#if !HAVE_STRLCPY
#include "strlcpy.h"
#endif

#if !HAVE_STRLCAT
#include "strlcat.h"
#endif

#ifndef O_PATH
#define O_PATH      010000000
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW  00400000
#endif

lxc_log_define(utils, lxc);

/*
 * if path is btrfs, tries to remove it and any subvolumes beneath it
 */
extern bool btrfs_try_remove_subvol(const char *path);

#ifdef IN_LIBLXC

static int _recursive_rmdir(const char *dirname, dev_t pdev,
			    const char *exclude, int level, bool onedev)
{
	__do_closedir DIR *dir = NULL;
	int failed = 0;
	bool hadexclude = false;
	int ret;
	struct dirent *direntp;
	char pathname[PATH_MAX];

	dir = opendir(dirname);
	if (!dir)
		return log_error(-1, "Failed to open \"%s\"", dirname);

	while ((direntp = readdir(dir))) {
		int rc;
		struct stat mystat;

		if (strequal(direntp->d_name, ".") ||
		    strequal(direntp->d_name, ".."))
			continue;

		rc = strnprintf(pathname, sizeof(pathname), "%s/%s", dirname, direntp->d_name);
		if (rc < 0) {
			ERROR("The name of path is too long");
			failed = 1;
			continue;
		}

		if (!level && exclude && strequal(direntp->d_name, exclude)) {
			ret = rmdir(pathname);
			if (ret < 0) {
				switch (errno) {
				case ENOTEMPTY:
					INFO("Not deleting snapshot \"%s\"", pathname);
					hadexclude = true;
					break;
				case ENOTDIR:
					ret = unlink(pathname);
					if (ret)
						INFO("Failed to remove \"%s\"", pathname);
					break;
				default:
					SYSERROR("Failed to rmdir \"%s\"", pathname);
					failed = 1;
					break;
				}
			}

			continue;
		}

		ret = lstat(pathname, &mystat);
		if (ret) {
			SYSERROR("Failed to stat \"%s\"", pathname);
			failed = 1;
			continue;
		}

		if (onedev && mystat.st_dev != pdev) {
			if (btrfs_try_remove_subvol(pathname))
				INFO("Removed btrfs subvolume at \"%s\"", pathname);
			continue;
		}

		if (S_ISDIR(mystat.st_mode)) {
			if (_recursive_rmdir(pathname, pdev, exclude, level + 1, onedev) < 0)
				failed = 1;
		} else {
			ret = unlink(pathname);
			if (ret < 0) {
				__do_close int fd = -EBADF;

				fd = open(pathname, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
				if (fd >= 0) {
					/* The file might be marked immutable. */
					int attr = 0;
					ret = ioctl(fd, FS_IOC_GETFLAGS, &attr);
					if (ret < 0)
						SYSERROR("Failed to retrieve file flags");
					attr &= ~FS_IMMUTABLE_FL;
					ret = ioctl(fd, FS_IOC_SETFLAGS, &attr);
					if (ret < 0)
						SYSERROR("Failed to set file flags");
				}

				ret = unlink(pathname);
				if (ret < 0) {
					SYSERROR("Failed to delete \"%s\"", pathname);
					failed = 1;
				}
			}
		}
	}

	if (rmdir(dirname) < 0 && !btrfs_try_remove_subvol(dirname) && !hadexclude) {
		SYSERROR("Failed to delete \"%s\"", dirname);
		failed = 1;
	}

	return failed ? -1 : 0;
}

/*
 * In overlayfs, st_dev is unreliable. So on overlayfs we don't do the
 * lxc_rmdir_onedev().
 */
static inline bool is_native_overlayfs(const char *path)
{
	return has_fs_type(path, OVERLAY_SUPER_MAGIC) ||
	       has_fs_type(path, OVERLAYFS_SUPER_MAGIC);
}

/* returns 0 on success, -1 if there were any failures */
extern int lxc_rmdir_onedev(const char *path, const char *exclude)
{
	struct stat mystat;
	bool onedev = true;

	if (is_native_overlayfs(path))
		onedev = false;

	if (lstat(path, &mystat) < 0) {
		if (errno == ENOENT)
			return 0;

		return log_error_errno(-1, errno, "Failed to stat \"%s\"", path);
	}

	return _recursive_rmdir(path, mystat.st_dev, exclude, 0, onedev);
}

#endif /* IN_LIBLXC */

/* borrowed from iproute2 */
extern int get_u16(unsigned short *val, const char *arg, int base)
{
	unsigned long res;
	char *ptr;

	if (!arg || !*arg)
		return ret_errno(EINVAL);

	errno = 0;
	res = strtoul(arg, &ptr, base);
	if (!ptr || ptr == arg || *ptr || res > 0xFFFF || errno != 0)
		return ret_errno(ERANGE);

	*val = res;

	return 0;
}

int lxc_mkdir_p(const char *dir, mode_t mode)
{
	const char *tmp = dir;
	const char *orig = dir;

	if (access(dir, F_OK) != -1)
		return 0;

	do {
		__do_free char *makeme = NULL;
		int ret;

		dir = tmp + strspn(tmp, "/");
		tmp = dir + strcspn(dir, "/");

		makeme = strndup(orig, dir - orig);
		if (!makeme)
			return ret_set_errno(-1, ENOMEM);

		ret = mkdir(makeme, mode);
		if (ret < 0 && errno != EEXIST)
			return log_error_errno(-1, errno, "Failed to create directory \"%s\"", makeme);

	} while (tmp != dir);

	return 0;
}

char *get_rundir(void)
{
	__do_free char *rundir = NULL;
	char *static_rundir;
	int ret;
	size_t len;
	const char *homedir;
	struct stat sb;

	if (stat(RUNTIME_PATH, &sb) < 0)
		return NULL;

	if (geteuid() == sb.st_uid || getegid() == sb.st_gid)
		return strdup(RUNTIME_PATH);

	static_rundir = getenv("XDG_RUNTIME_DIR");
	if (static_rundir)
		return strdup(static_rundir);

	INFO("XDG_RUNTIME_DIR isn't set in the environment");
	homedir = getenv("HOME");
	if (!homedir)
		return log_error(NULL, "HOME isn't set in the environment");

	len = strlen(homedir) + 17;
	rundir = malloc(sizeof(char) * len);
	if (!rundir)
		return NULL;

	ret = strnprintf(rundir, len, "%s/.cache/lxc/run/", homedir);
	if (ret < 0)
		return ret_set_errno(NULL, EIO);

	return move_ptr(rundir);
}

int wait_for_pid(pid_t pid)
{
	int status, ret;

again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;

		return -1;
	}

	if (ret != pid)
		goto again;

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;

	return 0;
}

int wait_for_pidfd(int pidfd)
{
	int ret;
	siginfo_t info = {
		.si_signo = 0,
	};

	do {
		ret = waitid(P_PIDFD, pidfd, &info, __WALL | WEXITED);
	} while (ret < 0 && errno == EINTR);

	return !ret && WIFEXITED(info.si_status) && WEXITSTATUS(info.si_status) == 0;
}

int lxc_wait_for_pid_status(pid_t pid)
{
	int status, ret;

again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;

		return -1;
	}

	if (ret != pid)
		goto again;

	return status;
}

bool wait_exited(pid_t pid)
{
	int status;

	status = lxc_wait_for_pid_status(pid);
	if (status < 0)
		return log_error(false, "Failed to reap on child process %d", pid);
	if (WIFSIGNALED(status))
		return log_error(false, "Child process %d terminated by signal %d", pid, WTERMSIG(status));
	if (!WIFEXITED(status))
		return log_error(false, "Child did not termiate correctly");
	if (WEXITSTATUS(status))
		return log_error(false, "Child terminated with error %d", WEXITSTATUS(status));

	TRACE("Reaped child process %d", pid);
	return true;
}

#if HAVE_OPENSSL
#include <openssl/evp.h>

static int do_sha1_hash(const char *buf, int buflen, unsigned char *md_value,
			unsigned int *md_len)
{
	EVP_MD_CTX *mdctx;
	const EVP_MD *md;

	md = EVP_get_digestbyname("sha1");
	if (!md)
		return log_error(-1, "Unknown message digest: sha1\n");

	mdctx = EVP_MD_CTX_create();
	EVP_DigestInit_ex(mdctx, md, NULL);
	EVP_DigestUpdate(mdctx, buf, buflen);
	EVP_DigestFinal_ex(mdctx, md_value, md_len);
	EVP_MD_CTX_destroy(mdctx);

	return 0;
}

int sha1sum_file(char *fnam, unsigned char *digest, unsigned int *md_len)
{
	__do_free char *buf = NULL;
	__do_fclose FILE *f = NULL;
	int ret;
	ssize_t flen;
	ssize_t nbytes;

	if (!fnam)
		return -1;

	f = fopen_cloexec(fnam, "r");
	if (!f)
		return log_error_errno(-1, errno, "Failed to open template \"%s\"", fnam);

	if (fseek(f, 0, SEEK_END) < 0)
		return log_error_errno(-1, errno, "Failed to seek to end of template");

	flen = ftell(f);
	if (flen < 0)
		return log_error_errno(-1, errno, "Failed to tell size of template");

	if (fseek(f, 0, SEEK_SET) < 0)
		return log_error_errno(-1, errno, "Failed to seek to start of template");

	buf = malloc(flen + 1);
	if (!buf)
		return log_error_errno(-1, ENOMEM, "Out of memory");

	nbytes = fread(buf, 1, flen, f);
	if (nbytes < 0 || nbytes != flen)
		return log_error_errno(-1, errno, "Failed to read template");

	buf[flen] = '\0';
	ret = do_sha1_hash(buf, flen, (void *)digest, md_len);
	return ret;
}
#endif

struct lxc_popen_FILE *lxc_popen(const char *command)
{
	int ret;
	int pipe_fds[2];
	pid_t child_pid;
	struct lxc_popen_FILE *fp = NULL;

	ret = pipe2(pipe_fds, O_CLOEXEC);
	if (ret < 0)
		return NULL;

	child_pid = fork();
	if (child_pid < 0)
		goto on_error;

	if (!child_pid) {
		sigset_t mask;

		close(pipe_fds[0]);

		/* duplicate stdout */
		if (pipe_fds[1] != STDOUT_FILENO)
			ret = dup2(pipe_fds[1], STDOUT_FILENO);
		else
			ret = fcntl(pipe_fds[1], F_SETFD, 0);
		if (ret < 0) {
			close(pipe_fds[1]);
			_exit(EXIT_FAILURE);
		}

		/* duplicate stderr */
		if (pipe_fds[1] != STDERR_FILENO)
			ret = dup2(pipe_fds[1], STDERR_FILENO);
		else
			ret = fcntl(pipe_fds[1], F_SETFD, 0);
		close(pipe_fds[1]);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		/* unblock all signals */
		ret = sigfillset(&mask);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		ret = pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		/* check if /bin/sh exist, otherwise try Android location /system/bin/sh */
		if (file_exists("/bin/sh"))
			execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		else
			execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);

		_exit(127);
	}

	close(pipe_fds[1]);
	pipe_fds[1] = -1;

	fp = malloc(sizeof(*fp));
	if (!fp)
		goto on_error;

	memset(fp, 0, sizeof(*fp));

	fp->child_pid = child_pid;
	fp->pipe = pipe_fds[0];

	/* From now on, closing fp->f will also close fp->pipe. So only ever
	 * call fclose(fp->f).
	 */
	fp->f = fdopen(pipe_fds[0], "r");
	if (!fp->f)
		goto on_error;

	return fp;

on_error:
	/* We can only close pipe_fds[0] if fdopen() didn't succeed or wasn't
	 * called yet. Otherwise the fd belongs to the file opened by fdopen()
	 * since it isn't dup()ed.
	 */
	if (fp && !fp->f && pipe_fds[0] >= 0)
		close(pipe_fds[0]);

	if (pipe_fds[1] >= 0)
		close(pipe_fds[1]);

	if (fp && fp->f)
		fclose(fp->f);

	if (fp)
		free(fp);

	return NULL;
}

int lxc_pclose(struct lxc_popen_FILE *fp)
{
	pid_t wait_pid;
	int wstatus = 0;

	if (!fp)
		return -1;

	do {
		wait_pid = waitpid(fp->child_pid, &wstatus, 0);
	} while (wait_pid < 0 && errno == EINTR);

	fclose(fp->f);
	free(fp);

	if (wait_pid < 0)
		return -1;

	return wstatus;
}

static int run_buffer(char *buffer)
{
	__do_free char *output = NULL;
	__do_lxc_pclose struct lxc_popen_FILE *f = NULL;
	int fd, ret;

	f = lxc_popen(buffer);
	if (!f)
		return log_error_errno(-1, errno, "Failed to popen() %s", buffer);

	output = zalloc(LXC_LOG_BUFFER_SIZE);
	if (!output)
		return log_error_errno(-1, ENOMEM, "Failed to allocate memory for %s", buffer);

	fd = fileno(f->f);
	if (fd < 0)
		return log_error_errno(-1, errno, "Failed to retrieve underlying file descriptor");

	for (int i = 0; i < 10; i++) {
		ssize_t bytes_read;

		bytes_read = lxc_read_nointr(fd, output, LXC_LOG_BUFFER_SIZE - 1);
		if (bytes_read > 0) {
			output[bytes_read] = '\0';
			DEBUG("Script %s produced output: %s", buffer, output);
			continue;
		}

		break;
	}

	ret = lxc_pclose(move_ptr(f));
	if (ret == -1)
		return log_error_errno(-1, errno, "Script exited with error");
	else if (WIFEXITED(ret) && WEXITSTATUS(ret) != 0)
		return log_error(-1, "Script exited with status %d", WEXITSTATUS(ret));
	else if (WIFSIGNALED(ret))
		return log_error(-1, "Script terminated by signal %d", WTERMSIG(ret));

	return 0;
}

int run_script_argv(const char *name, unsigned int hook_version,
		    const char *section, const char *script,
		    const char *hookname, char **argv)
{
	__do_free char *buffer = NULL;
	int buf_pos, i, ret;
	size_t size = 0;

	if (hook_version == 0)
		INFO("Executing script \"%s\" for container \"%s\", config section \"%s\"",
		     script, name, section);
	else
		INFO("Executing script \"%s\" for container \"%s\"", script, name);

	for (i = 0; argv && argv[i]; i++)
		size += strlen(argv[i]) + 1;

	size += STRLITERALLEN("exec");
	size++;
	size += strlen(script);
	size++;

	if (size > INT_MAX)
		return -EFBIG;

	if (hook_version == 0) {
		size += strlen(hookname);
		size++;

		size += strlen(name);
		size++;

		size += strlen(section);
		size++;

		if (size > INT_MAX)
			return -EFBIG;
	}

	buffer = zalloc(size);
	if (!buffer)
		return -ENOMEM;

	if (hook_version == 0)
		buf_pos = strnprintf(buffer, size, "exec %s %s %s %s", script, name, section, hookname);
	else
		buf_pos = strnprintf(buffer, size, "exec %s", script);
	if (buf_pos < 0)
		return log_error_errno(-1, errno, "Failed to create command line for script \"%s\"", script);

	if (hook_version == 1) {
		ret = setenv("LXC_HOOK_TYPE", hookname, 1);
		if (ret < 0) {
			return log_error_errno(-1, errno, "Failed to set environment variable: LXC_HOOK_TYPE=%s", hookname);
		}
		TRACE("Set environment variable: LXC_HOOK_TYPE=%s", hookname);

		ret = setenv("LXC_HOOK_SECTION", section, 1);
		if (ret < 0)
			return log_error_errno(-1, errno, "Failed to set environment variable: LXC_HOOK_SECTION=%s", section);
		TRACE("Set environment variable: LXC_HOOK_SECTION=%s", section);

		if (strequal(section, "net")) {
			char *parent;

			if (!argv || !argv[0])
				return -1;

			ret = setenv("LXC_NET_TYPE", argv[0], 1);
			if (ret < 0)
				return log_error_errno(-1, errno, "Failed to set environment variable: LXC_NET_TYPE=%s", argv[0]);
			TRACE("Set environment variable: LXC_NET_TYPE=%s", argv[0]);

			parent = argv[1] ? argv[1] : "";

			if (strequal(argv[0], "macvlan")) {
				ret = setenv("LXC_NET_PARENT", parent, 1);
				if (ret < 0)
					return log_error_errno(-1, errno, "Failed to set environment variable: LXC_NET_PARENT=%s", parent);
				TRACE("Set environment variable: LXC_NET_PARENT=%s", parent);
			} else if (strequal(argv[0], "phys")) {
				ret = setenv("LXC_NET_PARENT", parent, 1);
				if (ret < 0)
					return log_error_errno(-1, errno, "Failed to set environment variable: LXC_NET_PARENT=%s", parent);
				TRACE("Set environment variable: LXC_NET_PARENT=%s", parent);
			} else if (strequal(argv[0], "veth")) {
				char *peer = argv[2] ? argv[2] : "";

				ret = setenv("LXC_NET_PEER", peer, 1);
				if (ret < 0)
					return log_error_errno(-1, errno, "Failed to set environment variable: LXC_NET_PEER=%s", peer);
				TRACE("Set environment variable: LXC_NET_PEER=%s", peer);

				ret = setenv("LXC_NET_PARENT", parent, 1);
				if (ret < 0)
					return log_error_errno(-1, errno, "Failed to set environment variable: LXC_NET_PARENT=%s", parent);
				TRACE("Set environment variable: LXC_NET_PARENT=%s", parent);
			}
		}
	}

	for (i = 0; argv && argv[i]; i++) {
		size_t len = size - buf_pos;

		ret = strnprintf(buffer + buf_pos, len, " %s", argv[i]);
		if (ret < 0)
			return log_error_errno(-1, errno, "Failed to create command line for script \"%s\"", script);
		buf_pos += ret;
	}

	return run_buffer(buffer);
}

int run_script(const char *name, const char *section, const char *script, ...)
{
	__do_free char *buffer = NULL;
	int ret;
	char *p;
	va_list ap;
	size_t size = 0;

	INFO("Executing script \"%s\" for container \"%s\", config section \"%s\"",
	     script, name, section);

	va_start(ap, script);
	while ((p = va_arg(ap, char *)))
		size += strlen(p) + 1;
	va_end(ap);

	size += STRLITERALLEN("exec");
	size += strlen(script);
	size += strlen(name);
	size += strlen(section);
	size += 4;

	if (size > INT_MAX)
		return -1;

	buffer = must_realloc(NULL, size);
	ret = strnprintf(buffer, size, "exec %s %s %s", script, name, section);
	if (ret < 0)
		return -1;

	va_start(ap, script);
	while ((p = va_arg(ap, char *))) {
		int len = size - ret;
		int rc;
		rc = strnprintf(buffer + ret, len, " %s", p);
		if (rc < 0) {
			va_end(ap);
			return -1;
		}
		ret += rc;
	}
	va_end(ap);

	return run_buffer(buffer);
}

int randseed(bool srand_it)
{
	__do_fclose FILE *f = NULL;
	/*
	 * srand pre-seed function based on /dev/urandom
	 */
	unsigned int seed = time(NULL) + getpid();

	f = fopen("/dev/urandom", "re");
	if (f) {
		int ret = fread(&seed, sizeof(seed), 1, f);
		if (ret != 1)
			SYSDEBUG("Unable to fread /dev/urandom, fallback to time+pid rand seed");
	}

	if (srand_it)
		srand(seed);

	return seed;
}

uid_t get_ns_uid(uid_t orig)
{
	__do_free char *line = NULL;
	__do_fclose FILE *f = NULL;
	size_t sz = 0;
	uid_t nsid, hostid, range;

	f = fopen("/proc/self/uid_map", "re");
	if (!f)
		return log_error_errno(0, errno, "Failed to open uid_map");

	while (getline(&line, &sz, f) != -1) {
		if (sscanf(line, "%u %u %u", &nsid, &hostid, &range) != 3)
			continue;

		if (hostid <= orig && hostid + range > orig)
			return nsid += orig - hostid;
	}

	return LXC_INVALID_UID;
}

gid_t get_ns_gid(gid_t orig)
{
	__do_free char *line = NULL;
	__do_fclose FILE *f = NULL;
	size_t sz = 0;
	gid_t nsid, hostid, range;

	f = fopen("/proc/self/gid_map", "re");
	if (!f)
		return log_error_errno(0, errno, "Failed to open gid_map");

	while (getline(&line, &sz, f) != -1) {
		if (sscanf(line, "%u %u %u", &nsid, &hostid, &range) != 3)
			continue;

		if (hostid <= orig && hostid + range > orig)
			return nsid += orig - hostid;
	}

	return LXC_INVALID_GID;
}

bool dir_exists(const char *path)
{
	return exists_dir_at(-1, path);
}

/* Note we don't use SHA-1 here as we don't want to depend on HAVE_GNUTLS.
 * FNV has good anti collision properties and we're not worried
 * about pre-image resistance or one-way-ness, we're just trying to make
 * the name unique in the 108 bytes of space we have.
 */
uint64_t fnv_64a_buf(void *buf, size_t len, uint64_t hval)
{
	unsigned char *bp;

	for(bp = buf; bp < (unsigned char *)buf + len; bp++) {
		/* xor the bottom with the current octet */
		hval ^= (uint64_t)*bp;

		/* gcc optimised:
		 * multiply by the 64 bit FNV magic prime mod 2^64
		 */
		hval += (hval << 1) + (hval << 4) + (hval << 5) +
			(hval << 7) + (hval << 8) + (hval << 40);
	}

	return hval;
}

bool is_shared_mountpoint(const char *path)
{
	__do_fclose FILE *f = NULL;
	__do_free char *line = NULL;
	int i;
	size_t len = 0;

	f = fopen("/proc/self/mountinfo", "re");
	if (!f)
		return 0;

	while (getline(&line, &len, f) > 0) {
		char *slider1, *slider2;

		for (slider1 = line, i = 0; slider1 && i < 4; i++)
			slider1 = strchr(slider1 + 1, ' ');

		if (!slider1)
			continue;

		slider2 = strchr(slider1 + 1, ' ');
		if (!slider2)
			continue;

		*slider2 = '\0';
		if (strequal(slider1 + 1, path)) {
			/* This is the path. Is it shared? */
			slider1 = strchr(slider2 + 1, ' ');
			if (slider1 && strstr(slider1, "shared:"))
				return true;
		}
	}

	return false;
}

/*
 * Detect whether / is mounted MS_SHARED.  The only way I know of to
 * check that is through /proc/self/mountinfo.
 * I'm only checking for /.  If the container rootfs or mount location
 * is MS_SHARED, but not '/', then you're out of luck - figuring that
 * out would be too much work to be worth it.
 */
int detect_shared_rootfs(void)
{
	if (is_shared_mountpoint("/"))
		return 1;

	return 0;
}

bool try_switch_to_ns(pid_t pid, const char *ns, bool optional)
{
	__do_close int fd = -EBADF;
	int ret;
	char nspath[STRLITERALLEN("/proc//ns/")
		    + INTTYPE_TO_STRLEN(pid_t)
		    + LXC_NAMESPACE_NAME_MAX];

	/* Switch to new ns */
	ret = strnprintf(nspath, sizeof(nspath), "/proc/%d/ns/%s", pid, ns);
	if (ret < 0)
		return false;

	fd = open(nspath, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return log_error_errno(false, errno, "Failed to open \"%s\"", nspath);

	ret = setns(fd, 0);
	if (ret) {
		if (optional)
			return log_trace_errno(false, errno, "Failed to set process %d to \"%s\" of %d", pid, ns, fd);
		else
			return log_error_errno(false, errno, "Failed to set process %d to \"%s\" of %d", pid, ns, fd);
	}

	return true;
}

/*
 * looking at fs/proc_namespace.c, it appears we can
 * actually expect the rootfs entry to very specifically contain
 * " - rootfs rootfs "
 * IIUC, so long as we've chrooted so that rootfs is not our root,
 * the rootfs entry should always be skipped in mountinfo contents.
 */
bool detect_ramfs_rootfs(void)
{
	__do_free char *line = NULL;
	__do_free void *fopen_cache = NULL;
	__do_fclose FILE *f = NULL;
	size_t len = 0;

	f = fopen_cached("/proc/self/mountinfo", "re", &fopen_cache);
	if (!f)
		return false;

	while (getline(&line, &len, f) != -1) {
		int i;
		char *p, *p2;

		for (p = line, i = 0; p && i < 4; i++)
			p = strchr(p + 1, ' ');
		if (!p)
			continue;

		p2 = strchr(p + 1, ' ');
		if (!p2)
			continue;
		*p2 = '\0';
		if (strequal(p + 1, "/")) {
			/* This is '/'. Is it the ramfs? */
			p = strchr(p2 + 1, '-');
			if (p && strnequal(p, "- rootfs ", 9))
				return true;
		}
	}

	return false;
}

char *on_path(const char *cmd, const char *rootfs)
{
	__do_free char *path = NULL;
	char *entry = NULL;
	char cmdpath[PATH_MAX];
	int ret;

	path = getenv("PATH");
	if (!path)
		return NULL;

	path = strdup(path);
	if (!path)
		return NULL;

	lxc_iterate_parts(entry, path, ":") {
		if (rootfs)
			ret = strnprintf(cmdpath, sizeof(cmdpath), "%s/%s/%s", rootfs, entry, cmd);
		else
			ret = strnprintf(cmdpath, sizeof(cmdpath), "%s/%s", entry, cmd);
		if (ret < 0)
			continue;

		if (access(cmdpath, X_OK) == 0)
			return strdup(cmdpath);
	}

	return NULL;
}

/*
 * Given the '-t' template option to lxc-create, figure out what to
 * do.  If the template is a full executable path, use that.  If it
 * is something like 'sshd', then return $templatepath/lxc-sshd.
 * On success return the template, on error return NULL.
 */
char *get_template_path(const char *t)
{
	int ret, len;
	char *tpath;

	if (t[0] == '/') {
		if (access(t, X_OK) == 0) {
			return strdup(t);
		} else {
			SYSERROR("Bad template pathname: %s", t);
			return NULL;
		}
	}

	len = strlen(LXCTEMPLATEDIR) + strlen(t) + strlen("/lxc-") + 1;

	tpath = malloc(len);
	if (!tpath)
		return NULL;

	ret = strnprintf(tpath, len, "%s/lxc-%s", LXCTEMPLATEDIR, t);
	if (ret < 0) {
		free(tpath);
		return NULL;
	}

	if (access(tpath, X_OK) < 0) {
		SYSERROR("bad template: %s", t);
		free(tpath);
		return NULL;
	}

	return tpath;
}

/*
 * @path:    a pathname where / replaced with '\0'.
 * @offsetp: pointer to int showing which path segment was last seen.
 *           Updated on return to reflect the next segment.
 * @fulllen: full original path length.
 * Returns a pointer to the next path segment, or NULL if done.
 */
static char *get_nextpath(char *path, int *offsetp, int fulllen)
{
	int offset = *offsetp;

	if (offset >= fulllen)
		return NULL;

	while (offset < fulllen && path[offset] != '\0')
		offset++;

	while (offset < fulllen && path[offset] == '\0')
		offset++;

	*offsetp = offset;

	return (offset < fulllen) ? &path[offset] : NULL;
}

/*
 * Check that @subdir is a subdir of @dir.  @len is the length of
 * @dir (to avoid having to recalculate it).
 */
static bool is_subdir(const char *subdir, const char *dir, size_t len)
{
	size_t subdirlen = strlen(subdir);

	if (subdirlen < len)
		return false;

	if (!strnequal(subdir, dir, len))
		return false;

	if (dir[len-1] == '/')
		return true;

	if (subdir[len] == '/' || subdirlen == len)
		return true;

	return false;
}

/*
 * Check if the open fd is a symlink.  Return -ELOOP if it is.  Return
 * -ENOENT if we couldn't fstat.  Return 0 if the fd is ok.
 */
static int check_symlink(int fd)
{
	struct stat sb;
	int ret;

	ret = fstat(fd, &sb);
	if (ret < 0)
		return -ENOENT;

	if (S_ISLNK(sb.st_mode))
		return -ELOOP;

	return 0;
}

/*
 * Open a file or directory, provided that it contains no symlinks.
 *
 * CAVEAT: This function must not be used for other purposes than container
 * setup before executing the container's init
 */
static int open_if_safe(int dirfd, const char *nextpath)
{
	int newfd = openat(dirfd, nextpath, O_RDONLY | O_NOFOLLOW);
	if (newfd >= 0) /* Was not a symlink, all good. */
		return newfd;

	if (errno == ELOOP)
		return newfd;

	if (errno == EPERM || errno == EACCES) {
		/* We're not root (cause we got EPERM) so try opening with
		 * O_PATH.
		 */
		newfd = openat(dirfd, nextpath, O_PATH | O_NOFOLLOW);
		if (newfd >= 0) {
			/* O_PATH will return an fd for symlinks. We know
			 * nextpath wasn't a symlink at last openat, so if fd is
			 * now a link, then something * fishy is going on.
			 */
			int ret = check_symlink(newfd);
			if (ret < 0) {
				close(newfd);
				newfd = ret;
			}
		}
	}

	return newfd;
}

/*
 * Open a path intending for mounting, ensuring that the final path
 * is inside the container's rootfs.
 *
 * CAVEAT: This function must not be used for other purposes than container
 * setup before executing the container's init
 *
 * @target: path to be opened
 * @prefix_skip: a part of @target in which to ignore symbolic links.  This
 * would be the container's rootfs.
 *
 * Return an open fd for the path, or <0 on error.
 */
static int open_without_symlink(const char *target, const char *prefix_skip)
{
	int curlen = 0, dirfd, fulllen, i;
	char *dup;

	fulllen = strlen(target);

	/* make sure prefix-skip makes sense */
	if (prefix_skip && strlen(prefix_skip) > 0) {
		curlen = strlen(prefix_skip);
		if (!is_subdir(target, prefix_skip, curlen)) {
			ERROR("WHOA there - target \"%s\" didn't start with prefix \"%s\"",
			      target, prefix_skip);
			return -EINVAL;
		}

		/*
		 * get_nextpath() expects the curlen argument to be
		 * on a  (turned into \0) / or before it, so decrement
		 * curlen to make sure that happens
		 */
		if (curlen)
			curlen--;
	} else {
		prefix_skip = "/";
		curlen = 0;
	}

	/* Make a copy of target which we can hack up, and tokenize it */
	if ((dup = strdup(target)) == NULL) {
		ERROR("Out of memory checking for symbolic link");
		return -ENOMEM;
	}

	for (i = 0; i < fulllen; i++) {
		if (dup[i] == '/')
			dup[i] = '\0';
	}

	dirfd = open(prefix_skip, O_RDONLY);
	if (dirfd < 0) {
		SYSERROR("Failed to open path \"%s\"", prefix_skip);
		goto out;
	}

	for (;;) {
		int newfd, saved_errno;
		char *nextpath;

		if ((nextpath = get_nextpath(dup, &curlen, fulllen)) == NULL)
			goto out;

		newfd = open_if_safe(dirfd, nextpath);
		saved_errno = errno;
		close(dirfd);

		dirfd = newfd;
		if (newfd < 0) {
			errno = saved_errno;
			if (errno == ELOOP)
				SYSERROR("%s in %s was a symbolic link!", nextpath, target);

			goto out;
		}
	}

out:
	free(dup);
	return dirfd;
}

int __safe_mount_beneath_at(int beneath_fd, const char *src, const char *dst, const char *fstype,
			    unsigned int flags, const void *data)
{
	__do_close int source_fd = -EBADF, target_fd = -EBADF;
	struct open_how how = {
		.flags		= PROTECT_OPATH_DIRECTORY,
		.resolve	= PROTECT_LOOKUP_BENEATH_WITH_MAGICLINKS,
	};
	int ret;
	char src_buf[LXC_PROC_PID_FD_LEN], tgt_buf[LXC_PROC_PID_FD_LEN];

	if (beneath_fd < 0)
		return -EINVAL;

	if ((flags & MS_BIND) && src && src[0] != '/') {
		source_fd = openat2(beneath_fd, src, &how, sizeof(how));
		if (source_fd < 0)
			return -errno;
		ret = strnprintf(src_buf, sizeof(src_buf), "/proc/self/fd/%d", source_fd);
		if (ret < 0)
			return -EIO;
	} else {
		src_buf[0] = '\0';
	}

	target_fd = openat2(beneath_fd, dst, &how, sizeof(how));
	if (target_fd < 0)
		return log_error_errno(-errno, errno, "Failed to open %d(%s)", beneath_fd, dst);
	ret = strnprintf(tgt_buf, sizeof(tgt_buf), "/proc/self/fd/%d", target_fd);
	if (ret < 0)
		return -EIO;

	if (!is_empty_string(src_buf))
		ret = mount(src_buf, tgt_buf, fstype, flags, data);
	else
		ret = mount(src, tgt_buf, fstype, flags, data);

	return ret;
}

int safe_mount_beneath(const char *beneath, const char *src, const char *dst, const char *fstype,
		       unsigned int flags, const void *data)
{
	__do_close int beneath_fd = -EBADF;
	const char *path = beneath ? beneath : "/";

	beneath_fd = openat(-1, path, PROTECT_OPATH_DIRECTORY);
	if (beneath_fd < 0)
		return log_error_errno(-errno, errno, "Failed to open %s", path);

	return __safe_mount_beneath_at(beneath_fd, src, dst, fstype, flags, data);
}

int safe_mount_beneath_at(int beneath_fd, const char *src, const char *dst, const char *fstype,
			  unsigned int flags, const void *data)
{
	return __safe_mount_beneath_at(beneath_fd, src, dst, fstype, flags, data);
}

/*
 * Safely mount a path into a container, ensuring that the mount target
 * is under the container's @rootfs.  (If @rootfs is NULL, then the container
 * uses the host's /)
 *
 * CAVEAT: This function must not be used for other purposes than container
 * setup before executing the container's init
 */
int safe_mount(const char *src, const char *dest, const char *fstype,
		unsigned long flags, const void *data, const char *rootfs)
{
	int destfd, ret, saved_errno;
	/* Only needs enough for /proc/self/fd/<fd>. */
	char srcbuf[50], destbuf[50];
	int srcfd = -1;
	const char *mntsrc = src;

	if (!rootfs)
		rootfs = "";

	/* todo - allow symlinks for relative paths if 'allowsymlinks' option is passed */
	if (flags & MS_BIND && src && src[0] != '/') {
		INFO("This is a relative bind mount");

		srcfd = open_without_symlink(src, NULL);
		if (srcfd < 0)
			return srcfd;

		ret = strnprintf(srcbuf, sizeof(srcbuf), "/proc/self/fd/%d", srcfd);
		if (ret < 0) {
			close(srcfd);
			ERROR("Out of memory");
			return -EINVAL;
		}
		mntsrc = srcbuf;
	}

	destfd = open_without_symlink(dest, rootfs);
	if (destfd < 0) {
		if (srcfd != -1) {
			saved_errno = errno;
			close(srcfd);
			errno = saved_errno;
		}

		return destfd;
	}

	ret = strnprintf(destbuf, sizeof(destbuf), "/proc/self/fd/%d", destfd);
	if (ret < 0) {
		if (srcfd != -1)
			close(srcfd);

		close(destfd);
		ERROR("Out of memory");
		return -EINVAL;
	}

	ret = mount(mntsrc, destbuf, fstype, flags, data);
	saved_errno = errno;
	if (srcfd != -1)
		close(srcfd);

	close(destfd);
	if (ret < 0) {
		errno = saved_errno;
		SYSERROR("Failed to mount \"%s\" onto \"%s\"", src ? src : "(null)", dest);
		return ret;
	}

	return 0;
}

int open_devnull(void)
{
	int fd = open("/dev/null", O_RDWR);
	if (fd < 0)
		SYSERROR("Can't open /dev/null");

	return fd;
}

int set_stdfds(int fd)
{
	int ret;

	if (fd < 0)
		return -1;

	ret = dup2(fd, STDIN_FILENO);
	if (ret < 0)
		return -1;

	ret = dup2(fd, STDOUT_FILENO);
	if (ret < 0)
		return -1;

	ret = dup2(fd, STDERR_FILENO);
	if (ret < 0)
		return -1;

	return 0;
}

int null_stdfds(void)
{
	int ret = -1;
	int fd;

	fd = open_devnull();
	if (fd >= 0) {
		ret = set_stdfds(fd);
		close(fd);
	}

	return ret;
}

/* Check whether a signal is blocked by a process. */
/* /proc/pid-to-str/status\0 = (5 + 21 + 7 + 1) */
#define __PROC_STATUS_LEN (6 + INTTYPE_TO_STRLEN(pid_t) + 7 + 1)
bool task_blocks_signal(pid_t pid, int signal)
{
	__do_free char *line = NULL;
	__do_fclose FILE *f = NULL;
	int ret;
	char status[__PROC_STATUS_LEN] = {0};
	uint64_t sigblk = 0, one = 1;
	size_t n = 0;
	bool bret = false;

	ret = strnprintf(status, sizeof(status), "/proc/%d/status", pid);
	if (ret < 0)
		return bret;

	f = fopen(status, "re");
	if (!f)
		return false;

	while (getline(&line, &n, f) != -1) {
		char *numstr;

		if (!strnequal(line, "SigBlk:", 7))
			continue;

		numstr = lxc_trim_whitespace_in_place(line + 7);
		ret = lxc_safe_uint64(numstr, &sigblk, 16);
		if (ret < 0)
			return false;

		break;
	}

	if (sigblk & (one << (signal - 1)))
		bret = true;

	return bret;
}

int lxc_preserve_ns(const int pid, const char *ns)
{
	int ret;
/* 5 /proc + 21 /int_as_str + 3 /ns + 20 /NS_NAME + 1 \0 */
#define __NS_PATH_LEN 50
	char path[__NS_PATH_LEN];

	/* This way we can use this function to also check whether namespaces
	 * are supported by the kernel by passing in the NULL or the empty
	 * string.
	 */
	ret = strnprintf(path, sizeof(path), "/proc/%d/ns%s%s", pid,
			 !ns || strequal(ns, "") ? "" : "/",
			 !ns || strequal(ns, "") ? "" : ns);
	if (ret < 0)
		return ret_errno(EIO);

	return open(path, O_RDONLY | O_CLOEXEC);
}

bool lxc_switch_uid_gid(uid_t uid, gid_t gid)
{
	int ret = 0;

	if (gid != LXC_INVALID_GID) {
		ret = setresgid(gid, gid, gid);
		if (ret < 0) {
			SYSERROR("Failed to switch to gid %d", gid);
			return false;
		}
		NOTICE("Switched to gid %d", gid);
	}

	if (uid != LXC_INVALID_UID) {
		ret = setresuid(uid, uid, uid);
		if (ret < 0) {
			SYSERROR("Failed to switch to uid %d", uid);
			return false;
		}
		NOTICE("Switched to uid %d", uid);
	}

	return true;
}

/* Simple convenience function which enables uniform logging. */
bool lxc_drop_groups(void)
{
	int ret;

	ret = setgroups(0, NULL);
	if (ret)
		return log_error_errno(false, errno, "Failed to drop supplimentary groups");

	NOTICE("Dropped supplimentary groups");
	return ret == 0;
}

bool lxc_setgroups(gid_t list[], size_t size)
{
	int ret;

	ret = setgroups(size, list);
	if (ret)
		return log_error_errno(false, errno, "Failed to set supplimentary groups");

	if (size > 0 && lxc_log_trace()) {
		for (size_t i = 0; i < size; i++)
			TRACE("Setting supplimentary group %d", list[i]);
	}

	NOTICE("Set supplimentary groups");
	return true;
}

static int lxc_get_unused_loop_dev_legacy(char *loop_name)
{
	struct dirent *dp;
	struct loop_info64 lo64;
	DIR *dir;
	int dfd = -1, fd = -1, ret = -1;

	dir = opendir("/dev");
	if (!dir) {
		SYSERROR("Failed to open \"/dev\"");
		return -1;
	}

	while ((dp = readdir(dir))) {
		if (!strnequal(dp->d_name, "loop", 4))
			continue;

		dfd = dirfd(dir);
		if (dfd < 0)
			continue;

		fd = openat(dfd, dp->d_name, O_RDWR);
		if (fd < 0)
			continue;

		ret = ioctl(fd, LOOP_GET_STATUS64, &lo64);
		if (ret < 0) {
			if (ioctl(fd, LOOP_GET_STATUS64, &lo64) == 0 ||
			    errno != ENXIO) {
				close(fd);
				fd = -1;
				continue;
			}
		}

		ret = strnprintf(loop_name, LO_NAME_SIZE, "/dev/%s", dp->d_name);
		if (ret < 0) {
			close(fd);
			fd = -1;
			continue;
		}

		break;
	}

	closedir(dir);

	if (fd < 0)
		return -1;

	return fd;
}

static int lxc_get_unused_loop_dev(char *name_loop)
{
	int loop_nr, ret;
	int fd_ctl = -1, fd_tmp = -1;

	fd_ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (fd_ctl < 0) {
		SYSERROR("Failed to open loop control");
		return -ENODEV;
	}

	loop_nr = ioctl(fd_ctl, LOOP_CTL_GET_FREE);
	if (loop_nr < 0) {
		SYSERROR("Failed to get loop control");
		goto on_error;
	}

	ret = strnprintf(name_loop, LO_NAME_SIZE, "/dev/loop%d", loop_nr);
	if (ret < 0)
		goto on_error;

	fd_tmp = open(name_loop, O_RDWR | O_CLOEXEC);
	if (fd_tmp < 0) {
		/* on Android loop devices are moved under /dev/block, give it a shot */
		ret = strnprintf(name_loop, LO_NAME_SIZE, "/dev/block/loop%d", loop_nr);
                if (ret < 0)
                        goto on_error;

		fd_tmp = open(name_loop, O_RDWR | O_CLOEXEC);
		if (fd_tmp < 0)
			SYSERROR("Failed to open loop \"%s\"", name_loop);
	}

on_error:
	close(fd_ctl);
	return fd_tmp;
}

int lxc_prepare_loop_dev(const char *source, char *loop_dev, int flags)
{
	int ret;
	struct loop_info64 lo64;
	int fd_img = -1, fret = -1, fd_loop = -1;

	fd_loop = lxc_get_unused_loop_dev(loop_dev);
	if (fd_loop < 0) {
		if (fd_loop != -ENODEV)
			goto on_error;

		fd_loop = lxc_get_unused_loop_dev_legacy(loop_dev);
		if (fd_loop < 0)
			goto on_error;
	}

	fd_img = open(source, O_RDWR | O_CLOEXEC);
	if (fd_img < 0) {
		SYSERROR("Failed to open source \"%s\"", source);
		goto on_error;
	}

	ret = ioctl(fd_loop, LOOP_SET_FD, fd_img);
	if (ret < 0) {
		SYSERROR("Failed to set loop fd");
		goto on_error;
	}

	memset(&lo64, 0, sizeof(lo64));
	lo64.lo_flags = flags;

	strlcpy((char *)lo64.lo_file_name, source, LO_NAME_SIZE);

	ret = ioctl(fd_loop, LOOP_SET_STATUS64, &lo64);
	if (ret < 0) {
		SYSERROR("Failed to set loop status64");
		goto on_error;
	}

	fret = 0;

on_error:
	if (fd_img >= 0)
		close(fd_img);

	if (fret < 0 && fd_loop >= 0) {
		close(fd_loop);
		fd_loop = -1;
	}

	return fd_loop;
}

int lxc_unstack_mountpoint(const char *path, bool lazy)
{
	int ret;
	int umounts = 0;

pop_stack:
	ret = umount2(path, lazy ? MNT_DETACH : 0);
	if (ret < 0) {
		/* We consider anything else than EINVAL deadly to prevent going
		 * into an infinite loop. (The other alternative is constantly
		 * parsing /proc/self/mountinfo which is yucky and probably
		 * racy.)
		 */
		if (errno != EINVAL)
			return -errno;
	} else {
		/* Just stop counting when this happens. That'd just be so
		 * stupid that we won't even bother trying to report back the
		 * correct value anymore.
		 */
		if (umounts != INT_MAX)
			umounts++;

		/* We succeeded in umounting. Make sure that there's no other
		 * mountpoint stacked underneath.
		 */
		goto pop_stack;
	}

	return umounts;
}

static int run_command_internal(char *buf, size_t buf_size, int (*child_fn)(void *), void *args, bool wait_status)
{
	pid_t child;
	int ret, fret, pipefd[2];
	ssize_t bytes;

	/* Make sure our callers do not receive uninitialized memory. */
	if (buf_size > 0 && buf)
		buf[0] = '\0';

	if (pipe(pipefd) < 0) {
		SYSERROR("Failed to create pipe");
		return -1;
	}

	child = lxc_raw_clone(0, NULL);
	if (child < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		SYSERROR("Failed to create new process");
		return -1;
	}

	if (child == 0) {
		/* Close the read-end of the pipe. */
		close(pipefd[0]);

		/* Redirect std{err,out} to write-end of the
		 * pipe.
		 */
		ret = dup2(pipefd[1], STDOUT_FILENO);
		if (ret >= 0)
			ret = dup2(pipefd[1], STDERR_FILENO);

		/* Close the write-end of the pipe. */
		close(pipefd[1]);

		if (ret < 0) {
			SYSERROR("Failed to duplicate std{err,out} file descriptor");
			_exit(EXIT_FAILURE);
		}

		/* Does not return. */
		child_fn(args);
		ERROR("Failed to exec command");
		_exit(EXIT_FAILURE);
	}

	/* close the write-end of the pipe */
	close(pipefd[1]);

	if (buf && buf_size > 0) {
		bytes = lxc_read_nointr(pipefd[0], buf, buf_size - 1);
		if (bytes > 0)
			buf[bytes - 1] = '\0';
	}

	if (wait_status)
		fret = lxc_wait_for_pid_status(child);
	else
		fret = wait_for_pid(child);

	/* close the read-end of the pipe */
	close(pipefd[0]);

	return fret;
}

int run_command(char *buf, size_t buf_size, int (*child_fn)(void *), void *args)
{
    return run_command_internal(buf, buf_size, child_fn, args, false);
}

int run_command_status(char *buf, size_t buf_size, int (*child_fn)(void *), void *args)
{
    return run_command_internal(buf, buf_size, child_fn, args, true);
}

bool lxc_nic_exists(char *nic)
{
#define __LXC_SYS_CLASS_NET_LEN 15 + IFNAMSIZ + 1
	char path[__LXC_SYS_CLASS_NET_LEN];
	int ret;
	struct stat sb;

	if (strequal(nic, "none"))
		return true;

	ret = strnprintf(path, sizeof(path), "/sys/class/net/%s", nic);
	if (ret < 0)
		return false;

	ret = stat(path, &sb);
	if (ret < 0)
		return false;

	return true;
}

uint64_t lxc_find_next_power2(uint64_t n)
{
	/* 0 is not valid input. We return 0 to the caller since 0 is not a
	 * valid power of two.
	 */
	if (n == 0)
		return 0;

	if (!(n & (n - 1)))
		return n;

	while (n & (n - 1))
		n = n & (n - 1);

	n = n << 1;
	return n;
}

static int process_dead(/* takes */ int status_fd)
{
	__do_close int dupfd = -EBADF;
	__do_free char *line = NULL;
	__do_fclose FILE *f = NULL;
	int ret = 0;
	size_t n = 0;

	dupfd = dup(status_fd);
	if (dupfd < 0)
		return -1;

	if (lxc_fd_cloexec(dupfd, true) < 0)
		return -1;

	f = fdopen(dupfd, "re");
	if (!f)
		return -1;

	/* Transfer ownership of fd. */
	move_fd(dupfd);

	ret = 0;
	while (getline(&line, &n, f) != -1) {
		char *state;

		if (!strnequal(line, "State:", 6))
			continue;

		state = lxc_trim_whitespace_in_place(line + 6);
		/* only check whether process is dead or zombie for now */
		if (*state == 'X' || *state == 'Z')
			ret = 1;
	}

	return ret;
}

int lxc_set_death_signal(int signal, pid_t parent, int parent_status_fd)
{
	int ret;
	pid_t ppid;

	ret = prctl(PR_SET_PDEATHSIG, prctl_arg(signal), prctl_arg(0),
		    prctl_arg(0), prctl_arg(0));

	/* verify that we haven't been orphaned in the meantime */
	ppid = (pid_t)syscall(SYS_getppid);
	if (ppid == 0) { /* parent outside our pidns */
		if (parent_status_fd < 0)
			return 0;

		if (process_dead(parent_status_fd) == 1)
			return raise(SIGKILL);
	} else if (ppid != parent) {
		return raise(SIGKILL);
	}

	if (ret < 0)
		return -1;

	return 0;
}

int lxc_rm_rf(const char *dirname)
{
	__do_closedir DIR *dir = NULL;
	int fret = 0;
	int ret;
	struct dirent *direntp;

	dir = opendir(dirname);
	if (!dir)
		return log_error_errno(-1, errno, "Failed to open dir \"%s\"", dirname);

	while ((direntp = readdir(dir))) {
		__do_free char *pathname = NULL;
		struct stat mystat;

		if (strequal(direntp->d_name, ".") ||
		    strequal(direntp->d_name, ".."))
			continue;

		pathname = must_make_path(dirname, direntp->d_name, NULL);
		ret = lstat(pathname, &mystat);
		if (ret < 0) {
			if (!fret)
				SYSWARN("Failed to stat \"%s\"", pathname);

			fret = -1;
			continue;
		}

		if (!S_ISDIR(mystat.st_mode))
			continue;

		ret = lxc_rm_rf(pathname);
		if (ret < 0)
			fret = -1;
	}

	ret = rmdir(dirname);
	if (ret < 0)
		return log_warn_errno(-1, errno, "Failed to delete \"%s\"", dirname);

	return fret;
}

bool lxc_can_use_pidfd(int pidfd)
{
	int ret;

	if (pidfd < 0)
		return log_trace(false, "Kernel does not support pidfds");

	/*
	 * We don't care whether or not children were in a waitable state. We
	 * just care whether waitid() recognizes P_PIDFD.
	 *
	 * Btw, while I have your attention, the above waitid() code is an
	 * excellent example of how _not_ to do flag-based kernel APIs. So if
	 * you ever go into kernel development or are already and you add this
	 * kind of flag potpourri even though you have read this comment shame
	 * on you. May the gods of operating system development have mercy on
	 * your soul because I won't.
	 */
	ret = waitid(P_PIDFD, pidfd, NULL,
		    /* Type of children to wait for. */
		    __WALL |
		    /* How to wait for them. */
		    WNOHANG | WNOWAIT |
		    /* What state to wait for. */
		    WEXITED | WSTOPPED | WCONTINUED);
	if (ret < 0)
		return log_error_errno(false, errno, "Kernel does not support waiting on processes through pidfds");

	ret = lxc_raw_pidfd_send_signal(pidfd, 0, NULL, 0);
	if (ret)
		return log_error_errno(false, errno, "Kernel does not support sending singals through pidfds");

	return log_trace(true, "Kernel supports pidfds");
}

int fix_stdio_permissions(uid_t uid)
{
	__do_close int devnull_fd = -EBADF;
	int fret = 0;
	int std_fds[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
	int ret;
	struct stat st, st_null;

	devnull_fd = open_devnull();
	if (devnull_fd < 0)
		return log_trace_errno(-1, errno, "Failed to open \"/dev/null\"");

	ret = fstat(devnull_fd, &st_null);
	if (ret)
		return log_trace_errno(-errno, errno, "Failed to stat \"/dev/null\"");

	for (size_t i = 0; i < ARRAY_SIZE(std_fds); i++) {
		ret = fstat(std_fds[i], &st);
		if (ret) {
			SYSWARN("Failed to stat standard I/O file descriptor %d", std_fds[i]);
			fret = -1;
			continue;
		}

		if (st.st_rdev == st_null.st_rdev)
			continue;

		ret = fchown(std_fds[i], uid, st.st_gid);
		if (ret) {
			SYSTRACE("Failed to chown standard I/O file descriptor %d to uid %d and gid %d",
			         std_fds[i], uid, st.st_gid);
			fret = -1;
			continue;
		}

		ret = fchmod(std_fds[i], 0600);
		if (ret) {
			SYSTRACE("Failed to chmod standard I/O file descriptor %d", std_fds[i]);
			fret = -1;
		}
	}

	return fret;
}

int print_r(int fd, const char *path)
{
	__do_close int dfd = -EBADF, dfd_dup = -EBADF;
	__do_closedir DIR *dir = NULL;
	int ret = 0;
	struct dirent *direntp;
	struct stat st;

	if (is_empty_string(path)) {
		char buf[LXC_PROC_SELF_FD_LEN];

		ret = strnprintf(buf, sizeof(buf), "/proc/self/fd/%d", fd);
		if (ret < 0)
			return ret_errno(EIO);

		/*
		 * O_PATH file descriptors can't be used so we need to re-open
		 * just in case.
		 */
		dfd = openat(-EBADF, buf, O_CLOEXEC | O_DIRECTORY, 0);
	} else {
		dfd = openat(fd, path, O_CLOEXEC | O_DIRECTORY, 0);
	}
	if (dfd < 0)
		return -1;

	dfd_dup = dup_cloexec(dfd);
	if (dfd_dup < 0)
		return -1;

	dir = fdopendir(dfd);
	if (!dir)
		return -1;
	/* Transfer ownership to fdopendir(). */
	move_fd(dfd);

	while ((direntp = readdir(dir))) {
		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;

		ret = fstatat(dfd_dup, direntp->d_name, &st, AT_SYMLINK_NOFOLLOW);
		if (ret < 0 && errno != ENOENT)
			break;

		ret = 0;
		if (S_ISDIR(st.st_mode))
			ret = print_r(dfd_dup, direntp->d_name);
		else
			INFO("mode(%o):uid(%d):gid(%d) -> %d/%s\n",
			     (st.st_mode & ~S_IFMT), st.st_uid, st.st_gid, dfd_dup,
			     direntp->d_name);
		if (ret < 0 && errno != ENOENT)
			break;
	}

	if (is_empty_string(path))
		ret = fstatat(fd, "", &st, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
	else
		ret = fstatat(fd, path, &st, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW);
	if (ret)
		return -1;
	else
		INFO("mode(%o):uid(%d):gid(%d) -> %s",
		     (st.st_mode & ~S_IFMT), st.st_uid, st.st_gid, maybe_empty(path));
	return ret;
}

uint64_t get_fssize(char *s)
{
	uint64_t ret;
	char *end;

	ret = strtoull(s, &end, 0);
	if (end == s) {
		ERROR("Invalid blockdev size '%s', using default size", s);
		return 0;
	}

	while (isblank(*end))
		end++;

	if (*end == '\0') {
		ret *= 1024ULL * 1024ULL; /* MB by default */
	} else if (*end == 'b' || *end == 'B') {
		ret *= 1ULL;
	} else if (*end == 'k' || *end == 'K') {
		ret *= 1024ULL;
	} else if (*end == 'm' || *end == 'M') {
		ret *= 1024ULL * 1024ULL;
	} else if (*end == 'g' || *end == 'G') {
		ret *= 1024ULL * 1024ULL * 1024ULL;
	} else if (*end == 't' || *end == 'T') {
		ret *= 1024ULL * 1024ULL * 1024ULL * 1024ULL;
	} else {
		ERROR("Invalid blockdev unit size '%c' in '%s', using default size", *end, s);
		return 0;
	}

	return ret;
}
