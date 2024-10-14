#include "system.h"

#include <unordered_map>

#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <array>
#include <map>
#include <vector>

#if defined(__linux__)
#include <linux/fs.h>		/* For FICLONERANGE */
#endif

#include <rpm/rpmte.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmplugin.h>
#include <rpm/rpmfileutil.h>
#include <rpm/rpmextents_internal.h>

#include "debug.h"

#include <sys/ioctl.h>

using inodeIndexHash = std::unordered_map <rpm_ino_t, const char *>;

/* We use this in find to indicate a key wasn't found. This is an
 * unrecoverable error, but we can at least show a decent error. 0 is never a
 * valid offset because it's the offset of the start of the file.
 */
#define NOT_FOUND 0

#define BUFFER_SIZE (1024 * 128)

struct reflink_state_s {
	reflink_state_s():fundamental_block_size(0), keys(0),
	    keysize(0), fd(0), files(NULL), transcoded(0) {
	}
	/* Stuff that's used across rpms */ long fundamental_block_size;
	std::array<char, BUFFER_SIZE> buffer;

	/* stuff that's used/updated per psm */
	uint32_t keys, keysize;

	/* table for current rpm, keys * (keysize + sizeof(rpm_loff_t)) */
	std::map<std::vector<unsigned char>, rpm_loff_t> table;
	FD_t fd;
	rpmfiles files;
	inodeIndexHash inodeIndexes;
	int transcoded;
};

typedef struct reflink_state_s *reflink_state;

static rpmRC reflink_init(rpmPlugin plugin, rpmts ts)
{
	reflink_state state = new reflink_state_s;

	/* IOCTL-FICLONERANGE(2): ...Disk filesystems generally require the offset
	 * and length arguments to be aligned to the fundamental block size.
	 *
	 * The value of "fundamental block size" is directly related to the
	 * system's page size, so we should use that.
	 */
	state->fundamental_block_size = sysconf(_SC_PAGESIZE);
	rpmPluginSetData(plugin, state);

	return RPMRC_OK;
}

static void reflink_cleanup(rpmPlugin plugin)
{
	reflink_state state = (reflink_state) rpmPluginGetData(plugin);
	delete state;
}

static rpmRC reflink_psm_pre(rpmPlugin plugin, rpmte te)
{
	reflink_state state = (reflink_state) rpmPluginGetData(plugin);
	state->fd = rpmteFd(te);
	if (state->fd == 0) {
		rpmlog(RPMLOG_DEBUG, _("reflink: fd = 0, no install\n"));
		return RPMRC_OK;
	}
	rpm_loff_t current = Ftell(state->fd);
	extents_footer_t extents_footer;
	if (Fseek(state->fd, -(sizeof(extents_footer)), SEEK_END) < 0) {
		rpmlog(RPMLOG_ERR, _("reflink: failed to seek for magic\n"));
		if (Fseek(state->fd, current, SEEK_SET) < 0)
			/* yes this gets a bit repetitive */
			rpmlog(RPMLOG_ERR, _("reflink: unable to seek back to original location\n"));
		return RPMRC_FAIL;
	}
	/* tail of file contains offset_table, offset_checksums then magic */
	size_t len = sizeof(extents_footer);
	if (Fread(&extents_footer, sizeof(extents_footer), 1, state->fd) != sizeof(extents_footer)) {
		rpmlog(RPMLOG_ERR, _("reflink: unable to read trailer\n"));
		if (Fseek(state->fd, current, SEEK_SET) < 0)
			rpmlog(RPMLOG_ERR, _("reflink: unable to seek back to original location\n"));
		return RPMRC_FAIL;
	}
	if (extents_footer.magic != EXTENTS_MAGIC) {
		rpmlog(RPMLOG_DEBUG, _("reflink: not transcoded\n"));
		if (Fseek(state->fd, current, SEEK_SET) < 0) {
			rpmlog(RPMLOG_ERR,  _("reflink: unable to seek back to original location\n"));
			return RPMRC_FAIL;
		}
		return RPMRC_OK;
	}
	rpmlog(RPMLOG_DEBUG, _("reflink: *is* transcoded\n"));
	state->transcoded = 1;

	state->files = rpmteFiles(te);
	if (Fseek(state->fd, extents_footer.offsets.table_offset, SEEK_SET) < 0) {
		rpmlog(RPMLOG_ERR,
		       _("reflink: unable to seek to table_start\n"));
		return RPMRC_FAIL;
	}
	len = sizeof(state->keys);
	if (Fread(&state->keys, len, 1, state->fd) != len) {
		rpmlog(RPMLOG_ERR,
		       _("reflink: unable to read number of keys\n"));
		return RPMRC_FAIL;
	}
	len = sizeof(state->keysize);
	if (Fread(&state->keysize, len, 1, state->fd) != len) {
		rpmlog(RPMLOG_ERR, _("reflink: unable to read keysize\n"));
		return RPMRC_FAIL;
	}
	rpmlog(RPMLOG_DEBUG,
	       _("reflink: table_start=0x%lx, keys=%d, keysize=%d\n"),
	       extents_footer.offsets.table_offset, state->keys, state->keysize);
	/* now get digest table if there is a reason to have one. */
	if (state->keys == 0 || state->keysize == 0) {
		/* no files (or no digests(!)) */
		state->table.clear();
	} else {
		int entry_size = state->keysize + sizeof(rpm_loff_t);
		char entrybuf[entry_size];

		for (int i = 0; i < state->keys; i++) {
			if (Fread(entrybuf, entry_size, 1, state->fd) != entry_size) {
				rpmlog(RPMLOG_ERR, _("reflink: unable to read table\n"));
				return RPMRC_FAIL;
			}
			std::vector<unsigned char> digest(entrybuf, entrybuf + state->keysize);
			rpm_loff_t src_offset = *(rpm_loff_t *) (entrybuf + state->keysize);
			state->table[digest] = src_offset;
		}
		state->inodeIndexes.reserve(state->keys);
	}

	/* Seek back to original location.
	 * Might not be needed if we seek to offset immediately
	 */
	if (Fseek(state->fd, current, SEEK_SET) < 0) {
		rpmlog(RPMLOG_ERR, _("reflink: unable to seek back to original location\n"));
		return RPMRC_FAIL;
	}
	return RPMRC_OK;
}

static rpmRC reflink_psm_post(rpmPlugin plugin, rpmte te, int res)
{
	reflink_state state = (reflink_state) rpmPluginGetData(plugin);
	state->files = rpmfilesFree(state->files);
	state->table.clear();
	state->inodeIndexes.clear();
	state->transcoded = 0;
	return RPMRC_OK;
}

static rpmRC reflink_fsm_file_install(rpmPlugin plugin, rpmfi fi, int dirfd,
                                      const char *path, mode_t file_mode, rpmFsmOp op)
{
	struct file_clone_range fcr;
	rpm_loff_t size;
	int dst, rc;

	reflink_state state = (reflink_state) rpmPluginGetData(plugin);
	if (state->table.empty())
		/* no table means rpm is not in reflink format, so leave. Now. */
		return RPMRC_OK;
	if (op == FA_TOUCH)
		/* we're not overwriting an existing file. */
		return RPMRC_OK;
	fcr.dest_offset = 0;
	if (S_ISREG(file_mode) && !(rpmfiFFlags(fi) & RPMFILE_GHOST)) {
		rpm_ino_t inode = rpmfiFInode(fi);

		/* check for hard link entry in table. GetEntry overwrites hlix with
		 * the address of the first match.
		 */
		auto it = state->inodeIndexes.find(inode);
		if (it != state->inodeIndexes.end()) {
			const char *hl_target = it->second;
			/* entry is in table, use hard link */
			if (link(hl_target, rpmfiFN(fi)) != 0) {
				rpmlog(RPMLOG_ERR,
				       _("reflink: Unable to hard link %s -> %s due to %s\n"),
				       hl_target, rpmfiFN(fi), strerror(errno));
				return RPMRC_FAIL;
			}
			return RPMRC_PLUGIN_CONTENTS;
		}
		/* if we didn't hard link, then we'll track this inode as being
		 * created soon
		 */
		if (rpmfiFNlink(fi) > 1)
			/* minor optimization: only store files with more than one link */
			state->inodeIndexes.insert({inode, strdup(rpmfiFN(fi))});
		/* derived from wfd_open in fsm.c */
		mode_t old_umask = umask(0577);
		dst = openat(dirfd, path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR);
		umask(old_umask);
		if (dst == -1) {
			rpmlog(RPMLOG_ERR,
			       _("reflink: Unable to open %s for writing due to %s, flags = %x\n"),
			       rpmfiFN(fi), strerror(errno), rpmfiFFlags(fi));
			return RPMRC_FAIL;
		}
		size = rpmfiFSize(fi);
		if (size > 0) {
			/* round src_length down to fundamental_block_size multiple */
			fcr.src_length =
			    size / state->fundamental_block_size *
			    state->fundamental_block_size;
			if ((size % state->fundamental_block_size) > 0)
				/* round up to next fundamental_block_size. We expect the data
				 * in the rpm to be similarly padded.
				 */
				fcr.src_length += state->fundamental_block_size;
			fcr.src_fd = Fileno(state->fd);
			if (fcr.src_fd == -1) {
				close(dst);
				rpmlog(RPMLOG_ERR,
				       _("reflink: src fd lookup failed\n"));
				return RPMRC_FAIL;
			}
			const unsigned char *digest = rpmfiFDigest(fi, NULL, NULL);
			std::vector<unsigned char> key(digest, digest + state->keysize);
			auto src_offset = state->table.find(key);
			if (src_offset == state->table.end()) {
				close(dst);
				rpmlog(RPMLOG_ERR, _("reflink: digest not found\n"));
				return RPMRC_FAIL;
			}
			fcr.src_offset = src_offset->second;
			rpmlog(RPMLOG_DEBUG,
			       _("reflink: Reflinking %llu bytes at %llu to %s orig size=%ld, file=%lld\n"),
			       fcr.src_length, fcr.src_offset, rpmfiFN(fi), size,
			       fcr.src_fd);
			rc = ioctl(dst, FICLONERANGE, &fcr);
			if (rc) {
				rpmlog(RPMLOG_WARNING,
				       _("reflink: falling back to copying bits for %s due to %d, %d = %s\n"),
				       rpmfiFN(fi), rc, errno, strerror(errno));
				if (Fseek(state->fd, fcr.src_offset, SEEK_SET) < 0) {
					close(dst);
					rpmlog(RPMLOG_ERR, _("reflink: unable to seek on copying bits\n"));
					return RPMRC_FAIL;
				}
				rpm_loff_t left = size;
				size_t len, read, written;
				while (left) {
					len = (left > BUFFER_SIZE ? BUFFER_SIZE : left);
					read = Fread(state->buffer.data(), len, 1, state->fd);
					if (read != len) {
						close(dst);
						rpmlog(RPMLOG_ERR, _("reflink: short read on copying bits\n"));
						return RPMRC_FAIL;
					}
					written = write(dst, state->buffer.data(), len);
					if (read != written) {
						close(dst);
						rpmlog(RPMLOG_ERR, _("reflink: short write on copying bits\n"));
						return RPMRC_FAIL;
					}
					left -= len;
				}
			} else {
				/* reflink worked, so truncate */
				rc = ftruncate(dst, size);
				if (rc) {
					rpmlog(RPMLOG_ERR,
					       _("reflink: Unable to truncate %s to %ld due to %s\n"),
					       rpmfiFN(fi), size, strerror(errno));
					return RPMRC_FAIL;
				}
			}
		}
		close(dst);
		return RPMRC_PLUGIN_CONTENTS;
	}
	return RPMRC_OK;
}

static rpmRC reflink_fsm_file_archive_reader(rpmPlugin plugin, FD_t payload,
					     rpmfiles files, rpmfi *fi)
{
	reflink_state state = (reflink_state) rpmPluginGetData(plugin);
	if (state->transcoded) {
		*fi = rpmfilesIter(files, RPMFI_ITER_FWD);
		return RPMRC_PLUGIN_CONTENTS;
	}
	return RPMRC_OK;
}

struct rpmPluginHooks_s reflink_hooks = {
	.init = reflink_init,
	.cleanup = reflink_cleanup,
	.psm_pre = reflink_psm_pre,
	.psm_post = reflink_psm_post,
	.fsm_file_install = reflink_fsm_file_install,
	.fsm_file_archive_reader = reflink_fsm_file_archive_reader,
};
