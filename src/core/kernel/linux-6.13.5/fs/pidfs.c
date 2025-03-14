// SPDX-License-Identifier: GPL-2.0
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/cgroup.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pid.h>
#include <linux/pidfs.h>
#include <linux/pid_namespace.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/pseudo_fs.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <uapi/linux/pidfd.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/utsname.h>
#include <net/net_namespace.h>

#include "internal.h"
#include "mount.h"

#ifdef CONFIG_PROC_FS
/**
 * pidfd_show_fdinfo - print information about a pidfd
 * @m: proc fdinfo file
 * @f: file referencing a pidfd
 *
 * Pid:
 * This function will print the pid that a given pidfd refers to in the
 * pid namespace of the procfs instance.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its pid. This is
 * similar to calling getppid() on a process whose parent is outside of
 * its pid namespace.
 *
 * NSpid:
 * If pid namespaces are supported then this function will also print
 * the pid of a given pidfd refers to for all descendant pid namespaces
 * starting from the current pid namespace of the instance, i.e. the
 * Pid field and the first entry in the NSpid field will be identical.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its first NSpid
 * entry and no others will be shown.
 * Note that this differs from the Pid and NSpid fields in
 * /proc/<pid>/status where Pid and NSpid are always shown relative to
 * the  pid namespace of the procfs instance. The difference becomes
 * obvious when sending around a pidfd between pid namespaces from a
 * different branch of the tree, i.e. where no ancestral relation is
 * present between the pid namespaces:
 * - create two new pid namespaces ns1 and ns2 in the initial pid
 *   namespace (also take care to create new mount namespaces in the
 *   new pid namespace and mount procfs)
 * - create a process with a pidfd in ns1
 * - send pidfd from ns1 to ns2
 * - read /proc/self/fdinfo/<pidfd> and observe that both Pid and NSpid
 *   have exactly one entry, which is 0
 */
static void pidfd_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct pid *pid = pidfd_pid(f);
	struct pid_namespace *ns;
	pid_t nr = -1;

	if (likely(pid_has_task(pid, PIDTYPE_PID))) {
		ns = proc_pid_ns(file_inode(m->file)->i_sb);
		nr = pid_nr_ns(pid, ns);
	}

	seq_put_decimal_ll(m, "Pid:\t", nr);

#ifdef CONFIG_PID_NS
	seq_put_decimal_ll(m, "\nNSpid:\t", nr);
	if (nr > 0) {
		int i;

		/* If nr is non-zero it means that 'pid' is valid and that
		 * ns, i.e. the pid namespace associated with the procfs
		 * instance, is in the pid namespace hierarchy of pid.
		 * Start at one below the already printed level.
		 */
		for (i = ns->level + 1; i <= pid->level; i++)
			seq_put_decimal_ll(m, "\t", pid->numbers[i].nr);
	}
#endif
	seq_putc(m, '\n');
}
#endif

/*
 * Poll support for process exit notification.
 */
static __poll_t pidfd_poll(struct file *file, struct poll_table_struct *pts)
{
	struct pid *pid = pidfd_pid(file);
	bool thread = file->f_flags & PIDFD_THREAD;
	struct task_struct *task;
	__poll_t poll_flags = 0;

	poll_wait(file, &pid->wait_pidfd, pts);
	/*
	 * Depending on PIDFD_THREAD, inform pollers when the thread
	 * or the whole thread-group exits.
	 */
	guard(rcu)();
	task = pid_task(pid, PIDTYPE_PID);
	if (!task)
		poll_flags = EPOLLIN | EPOLLRDNORM | EPOLLHUP;
	else if (task->exit_state && (thread || thread_group_empty(task)))
		poll_flags = EPOLLIN | EPOLLRDNORM;

	return poll_flags;
}

static long pidfd_info(struct task_struct *task, unsigned int cmd, unsigned long arg)
{
	struct pidfd_info __user *uinfo = (struct pidfd_info __user *)arg;
	size_t usize = _IOC_SIZE(cmd);
	struct pidfd_info kinfo = {};
	struct user_namespace *user_ns;
	const struct cred *c;
	__u64 mask;
#ifdef CONFIG_CGROUPS
	struct cgroup *cgrp;
#endif

	if (!uinfo)
		return -EINVAL;
	if (usize < PIDFD_INFO_SIZE_VER0)
		return -EINVAL; /* First version, no smaller struct possible */

	if (copy_from_user(&mask, &uinfo->mask, sizeof(mask)))
		return -EFAULT;

	c = get_task_cred(task);
	if (!c)
		return -ESRCH;

	/* Unconditionally return identifiers and credentials, the rest only on request */

	user_ns = current_user_ns();
	kinfo.ruid = from_kuid_munged(user_ns, c->uid);
	kinfo.rgid = from_kgid_munged(user_ns, c->gid);
	kinfo.euid = from_kuid_munged(user_ns, c->euid);
	kinfo.egid = from_kgid_munged(user_ns, c->egid);
	kinfo.suid = from_kuid_munged(user_ns, c->suid);
	kinfo.sgid = from_kgid_munged(user_ns, c->sgid);
	kinfo.fsuid = from_kuid_munged(user_ns, c->fsuid);
	kinfo.fsgid = from_kgid_munged(user_ns, c->fsgid);
	kinfo.mask |= PIDFD_INFO_CREDS;
	put_cred(c);

#ifdef CONFIG_CGROUPS
	rcu_read_lock();
	cgrp = task_dfl_cgroup(task);
	kinfo.cgroupid = cgroup_id(cgrp);
	kinfo.mask |= PIDFD_INFO_CGROUPID;
	rcu_read_unlock();
#endif

	/*
	 * Copy pid/tgid last, to reduce the chances the information might be
	 * stale. Note that it is not possible to ensure it will be valid as the
	 * task might return as soon as the copy_to_user finishes, but that's ok
	 * and userspace expects that might happen and can act accordingly, so
	 * this is just best-effort. What we can do however is checking that all
	 * the fields are set correctly, or return ESRCH to avoid providing
	 * incomplete information. */

	kinfo.ppid = task_ppid_nr_ns(task, NULL);
	kinfo.tgid = task_tgid_vnr(task);
	kinfo.pid = task_pid_vnr(task);
	kinfo.mask |= PIDFD_INFO_PID;

	if (kinfo.pid == 0 || kinfo.tgid == 0 || (kinfo.ppid == 0 && kinfo.pid != 1))
		return -ESRCH;

	/*
	 * If userspace and the kernel have the same struct size it can just
	 * be copied. If userspace provides an older struct, only the bits that
	 * userspace knows about will be copied. If userspace provides a new
	 * struct, only the bits that the kernel knows about will be copied.
	 */
	if (copy_to_user(uinfo, &kinfo, min(usize, sizeof(kinfo))))
		return -EFAULT;

	return 0;
}

static bool pidfs_ioctl_valid(unsigned int cmd)
{
	switch (cmd) {
	case FS_IOC_GETVERSION:
	case PIDFD_GET_CGROUP_NAMESPACE:
	case PIDFD_GET_IPC_NAMESPACE:
	case PIDFD_GET_MNT_NAMESPACE:
	case PIDFD_GET_NET_NAMESPACE:
	case PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE:
	case PIDFD_GET_TIME_NAMESPACE:
	case PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE:
	case PIDFD_GET_UTS_NAMESPACE:
	case PIDFD_GET_USER_NAMESPACE:
	case PIDFD_GET_PID_NAMESPACE:
		return true;
	}

	/* Extensible ioctls require some more careful checks. */
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(PIDFD_GET_INFO):
		/*
		 * Try to prevent performing a pidfd ioctl when someone
		 * erronously mistook the file descriptor for a pidfd.
		 * This is not perfect but will catch most cases.
		 */
		return (_IOC_TYPE(cmd) == _IOC_TYPE(PIDFD_GET_INFO));
	}

	return false;
}

static long pidfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct task_struct *task __free(put_task) = NULL;
	struct nsproxy *nsp __free(put_nsproxy) = NULL;
	struct pid *pid = pidfd_pid(file);
	struct ns_common *ns_common = NULL;
	struct pid_namespace *pid_ns;

	if (!pidfs_ioctl_valid(cmd))
		return -ENOIOCTLCMD;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	/* Extensible IOCTL that does not open namespace FDs, take a shortcut */
	if (_IOC_NR(cmd) == _IOC_NR(PIDFD_GET_INFO))
		return pidfd_info(task, cmd, arg);

	if (arg)
		return -EINVAL;

	scoped_guard(task_lock, task) {
		nsp = task->nsproxy;
		if (nsp)
			get_nsproxy(nsp);
	}
	if (!nsp)
		return -ESRCH; /* just pretend it didn't exist */

	/*
	 * We're trying to open a file descriptor to the namespace so perform a
	 * filesystem cred ptrace check. Also, we mirror nsfs behavior.
	 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS))
		return -EACCES;

	switch (cmd) {
	/* Namespaces that hang of nsproxy. */
	case PIDFD_GET_CGROUP_NAMESPACE:
		if (IS_ENABLED(CONFIG_CGROUPS)) {
			get_cgroup_ns(nsp->cgroup_ns);
			ns_common = to_ns_common(nsp->cgroup_ns);
		}
		break;
	case PIDFD_GET_IPC_NAMESPACE:
		if (IS_ENABLED(CONFIG_IPC_NS)) {
			get_ipc_ns(nsp->ipc_ns);
			ns_common = to_ns_common(nsp->ipc_ns);
		}
		break;
	case PIDFD_GET_MNT_NAMESPACE:
		get_mnt_ns(nsp->mnt_ns);
		ns_common = to_ns_common(nsp->mnt_ns);
		break;
	case PIDFD_GET_NET_NAMESPACE:
		if (IS_ENABLED(CONFIG_NET_NS)) {
			ns_common = to_ns_common(nsp->net_ns);
			get_net_ns(ns_common);
		}
		break;
	case PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE:
		if (IS_ENABLED(CONFIG_PID_NS)) {
			get_pid_ns(nsp->pid_ns_for_children);
			ns_common = to_ns_common(nsp->pid_ns_for_children);
		}
		break;
	case PIDFD_GET_TIME_NAMESPACE:
		if (IS_ENABLED(CONFIG_TIME_NS)) {
			get_time_ns(nsp->time_ns);
			ns_common = to_ns_common(nsp->time_ns);
		}
		break;
	case PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE:
		if (IS_ENABLED(CONFIG_TIME_NS)) {
			get_time_ns(nsp->time_ns_for_children);
			ns_common = to_ns_common(nsp->time_ns_for_children);
		}
		break;
	case PIDFD_GET_UTS_NAMESPACE:
		if (IS_ENABLED(CONFIG_UTS_NS)) {
			get_uts_ns(nsp->uts_ns);
			ns_common = to_ns_common(nsp->uts_ns);
		}
		break;
	/* Namespaces that don't hang of nsproxy. */
	case PIDFD_GET_USER_NAMESPACE:
		if (IS_ENABLED(CONFIG_USER_NS)) {
			rcu_read_lock();
			ns_common = to_ns_common(get_user_ns(task_cred_xxx(task, user_ns)));
			rcu_read_unlock();
		}
		break;
	case PIDFD_GET_PID_NAMESPACE:
		if (IS_ENABLED(CONFIG_PID_NS)) {
			rcu_read_lock();
			pid_ns = task_active_pid_ns(task);
			if (pid_ns)
				ns_common = to_ns_common(get_pid_ns(pid_ns));
			rcu_read_unlock();
		}
		break;
	default:
		return -ENOIOCTLCMD;
	}

	if (!ns_common)
		return -EOPNOTSUPP;

	/* open_namespace() unconditionally consumes the reference */
	return open_namespace(ns_common);
}

static const struct file_operations pidfs_file_operations = {
	.poll		= pidfd_poll,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= pidfd_show_fdinfo,
#endif
	.unlocked_ioctl	= pidfd_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

struct pid *pidfd_pid(const struct file *file)
{
	if (file->f_op != &pidfs_file_operations)
		return ERR_PTR(-EBADF);
	return file_inode(file)->i_private;
}

static struct vfsmount *pidfs_mnt __ro_after_init;

#if BITS_PER_LONG == 32
/*
 * Provide a fallback mechanism for 32-bit systems so processes remain
 * reliably comparable by inode number even on those systems.
 */
static DEFINE_IDA(pidfd_inum_ida);

static int pidfs_inum(struct pid *pid, unsigned long *ino)
{
	int ret;

	ret = ida_alloc_range(&pidfd_inum_ida, RESERVED_PIDS + 1,
			      UINT_MAX, GFP_ATOMIC);
	if (ret < 0)
		return -ENOSPC;

	*ino = ret;
	return 0;
}

static inline void pidfs_free_inum(unsigned long ino)
{
	if (ino > 0)
		ida_free(&pidfd_inum_ida, ino);
}
#else
static inline int pidfs_inum(struct pid *pid, unsigned long *ino)
{
	*ino = pid->ino;
	return 0;
}
#define pidfs_free_inum(ino) ((void)(ino))
#endif

/*
 * The vfs falls back to simple_setattr() if i_op->setattr() isn't
 * implemented. Let's reject it completely until we have a clean
 * permission concept for pidfds.
 */
static int pidfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct iattr *attr)
{
	return -EOPNOTSUPP;
}


/*
 * User space expects pidfs inodes to have no file type in st_mode.
 *
 * In particular, 'lsof' has this legacy logic:
 *
 *	type = s->st_mode & S_IFMT;
 *	switch (type) {
 *	  ...
 *	case 0:
 *		if (!strcmp(p, "anon_inode"))
 *			Lf->ntype = Ntype = N_ANON_INODE;
 *
 * to detect our old anon_inode logic.
 *
 * Rather than mess with our internal sane inode data, just fix it
 * up here in getattr() by masking off the format bits.
 */
static int pidfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			 struct kstat *stat, u32 request_mask,
			 unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	stat->mode &= ~S_IFMT;
	return 0;
}

static const struct inode_operations pidfs_inode_operations = {
	.getattr = pidfs_getattr,
	.setattr = pidfs_setattr,
};

static void pidfs_evict_inode(struct inode *inode)
{
	struct pid *pid = inode->i_private;

	clear_inode(inode);
	put_pid(pid);
	pidfs_free_inum(inode->i_ino);
}

static const struct super_operations pidfs_sops = {
	.drop_inode	= generic_delete_inode,
	.evict_inode	= pidfs_evict_inode,
	.statfs		= simple_statfs,
};

/*
 * 'lsof' has knowledge of out historical anon_inode use, and expects
 * the pidfs dentry name to start with 'anon_inode'.
 */
static char *pidfs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	return dynamic_dname(buffer, buflen, "anon_inode:[pidfd]");
}

static const struct dentry_operations pidfs_dentry_operations = {
	.d_delete	= always_delete_dentry,
	.d_dname	= pidfs_dname,
	.d_prune	= stashed_dentry_prune,
};

static int pidfs_init_inode(struct inode *inode, void *data)
{
	inode->i_private = data;
	inode->i_flags |= S_PRIVATE;
	inode->i_mode |= S_IRWXU;
	inode->i_op = &pidfs_inode_operations;
	inode->i_fop = &pidfs_file_operations;
	/*
	 * Inode numbering for pidfs start at RESERVED_PIDS + 1. This
	 * avoids collisions with the root inode which is 1 for pseudo
	 * filesystems.
	 */
	return pidfs_inum(data, &inode->i_ino);
}

static void pidfs_put_data(void *data)
{
	struct pid *pid = data;
	put_pid(pid);
}

static const struct stashed_operations pidfs_stashed_ops = {
	.init_inode = pidfs_init_inode,
	.put_data = pidfs_put_data,
};

static int pidfs_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx;

	ctx = init_pseudo(fc, PID_FS_MAGIC);
	if (!ctx)
		return -ENOMEM;

	ctx->ops = &pidfs_sops;
	ctx->dops = &pidfs_dentry_operations;
	fc->s_fs_info = (void *)&pidfs_stashed_ops;
	return 0;
}

static struct file_system_type pidfs_type = {
	.name			= "pidfs",
	.init_fs_context	= pidfs_init_fs_context,
	.kill_sb		= kill_anon_super,
};

struct file *pidfs_alloc_file(struct pid *pid, unsigned int flags)
{

	struct file *pidfd_file;
	struct path path;
	int ret;

	ret = path_from_stashed(&pid->stashed, pidfs_mnt, get_pid(pid), &path);
	if (ret < 0)
		return ERR_PTR(ret);

	pidfd_file = dentry_open(&path, flags, current_cred());
	path_put(&path);
	return pidfd_file;
}

void __init pidfs_init(void)
{
	pidfs_mnt = kern_mount(&pidfs_type);
	if (IS_ERR(pidfs_mnt))
		panic("Failed to mount pidfs pseudo filesystem");
}
