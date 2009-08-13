/*-
 * Copyright (c) 2008-2009 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <xbps_api.h>

static int unpack_archive_fini(struct archive *, prop_dictionary_t, bool);
static void set_extract_flags(int);

int
xbps_unpack_binary_pkg(prop_dictionary_t pkg, bool essential)
{
	prop_string_t filename, repoloc, arch;
	struct archive *ar;
	const char *pkgname;
	char *binfile;
	int pkg_fd, rv = 0;

	assert(pkg != NULL);

	/*
	 * Append filename to the full path for binary pkg.
	 */
	prop_dictionary_get_cstring_nocopy(pkg, "pkgname", &pkgname);
	filename = prop_dictionary_get(pkg, "filename");
	arch = prop_dictionary_get(pkg, "architecture");
	repoloc = prop_dictionary_get(pkg, "repository");
	if (filename == NULL || arch == NULL || repoloc == NULL)
		return ENOTSUP;

	binfile = xbps_xasprintf("%s/%s/%s",
	    prop_string_cstring_nocopy(repoloc),
	    prop_string_cstring_nocopy(arch),
	    prop_string_cstring_nocopy(filename));
	if (binfile == NULL)
		return EINVAL;

	if ((pkg_fd = open(binfile, O_RDONLY)) == -1) {
		rv = errno;
		goto out;
	}

	ar = archive_read_new();
	if (ar == NULL) {
		rv = ENOMEM;
		goto out2;
	}

	/*
	 * Enable support for tar format and all compression methods.
	 */
	archive_read_support_compression_all(ar);
	archive_read_support_format_tar(ar);

	if ((rv = archive_read_open_fd(ar, pkg_fd,
	     ARCHIVE_READ_BLOCKSIZE)) != 0)
		goto out3;

	rv = unpack_archive_fini(ar, pkg, essential);
	/*
	 * If installation of package was successful, make sure the package
	 * is really on storage (if possible).
	 */
	if (rv == 0)
		if (fdatasync(pkg_fd) == -1)
			rv = errno;
out3:
	archive_read_finish(ar);
out2:
	(void)close(pkg_fd);
out:
	free(binfile);

	if (rv == 0) {
		/*
		 * Set package state to unpacked.
		 */
		rv = xbps_set_pkg_state_installed(pkgname,
		    XBPS_PKG_STATE_UNPACKED);
	}

	return rv;
}

/*
 * Flags for extracting files in binary packages. If a package
 * is marked as "essential", its files will be overwritten and then
 * the old and new dictionaries are compared to find out if there
 * are some files that were in the old package that should be removed.
 */
#define EXTRACT_FLAGS	ARCHIVE_EXTRACT_SECURE_NODOTDOT | \
			ARCHIVE_EXTRACT_SECURE_SYMLINKS
#define FEXTRACT_FLAGS	ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM | \
			ARCHIVE_EXTRACT_TIME | EXTRACT_FLAGS

static void
set_extract_flags(int flags)
{
	if (getuid() == 0)
		flags = FEXTRACT_FLAGS;
	else
		flags = EXTRACT_FLAGS;
}

static int
install_config_file(prop_dictionary_t d, struct archive_entry *entry, int flags)
{
	prop_object_t obj;
	prop_object_iterator_t iter;
	const char *cffile, *sha256;
	char *buf;
	int rv = 0;

	if (d == NULL)
		return 0;

	iter = xbps_get_array_iter_from_dict(d, "conf_files");
	if (iter == NULL)
		return 0;

	while ((obj = prop_object_iterator_next(iter))) {
		prop_dictionary_get_cstring_nocopy(obj, "file", &cffile);
		if (strstr(archive_entry_pathname(entry), cffile) == 0)
			continue;
		buf = xbps_xasprintf(".%s", cffile);
		if (buf == NULL) {
			prop_object_iterator_release(iter);
			return errno;
		}
		prop_dictionary_get_cstring_nocopy(obj, "sha256", &sha256);
		rv = xbps_check_file_hash(buf, sha256);
		free(buf);
		if (rv == 0 || rv == ENOENT) {
			/*
			 * Identical files or file not installed,
			 * install new file.
			 */
			rv = 0;
			break;
		} else if (rv == ERANGE) {
			/*
			 * Save new file with a .new extension,
			 * preserving current installed file.
			 */
			buf = xbps_xasprintf(".%s.new", cffile);
			if (buf == NULL) {
				prop_object_iterator_release(iter);
				return errno;
			}
			printf("Installing new configuration "
			    "file %s to %s\n", cffile, buf);

			set_extract_flags(flags);
			archive_entry_set_pathname(entry, buf);
			free(buf);
			rv = 0;
			break;
		} else {
			prop_object_iterator_release(iter);
			return rv;
		}
	}
	prop_object_iterator_release(iter);

	return rv;
}

/*
 * TODO: remove printfs and return appropiate errors to be interpreted by
 * the consumer.
 */
static int
unpack_archive_fini(struct archive *ar, prop_dictionary_t pkg,
		    bool essential)
{
	prop_dictionary_t filesd;
	struct archive_entry *entry;
	const char *pkgname, *version, *rootdir;
	char *buf, *buf2;
	int rv = 0, flags, lflags, eflags;
	bool actgt = false;

	assert(ar != NULL);
	assert(pkg != NULL);
	rootdir = xbps_get_rootdir();
	flags = xbps_get_flags();

	if (strcmp(rootdir, "") == 0)
		rootdir = "/";

	if (chdir(rootdir) == -1)
		return errno;

	prop_dictionary_get_cstring_nocopy(pkg, "pkgname", &pkgname);
	prop_dictionary_get_cstring_nocopy(pkg, "version", &version);

	set_extract_flags(lflags);

	while (archive_read_next_header(ar, &entry) == ARCHIVE_OK) {
		/*
		 * Always overwrite pkg metadata files. Other files
		 * in the archive aren't overwritten unless a package
		 * defines the "essential" boolean obj.
		 */
		eflags = 0;
		eflags = lflags;
		if (strcmp("./INSTALL", archive_entry_pathname(entry)) &&
		    strcmp("./REMOVE", archive_entry_pathname(entry)) &&
		    strcmp("./files.plist", archive_entry_pathname(entry)) &&
		    strcmp("./props.plist", archive_entry_pathname(entry))) {
			if (essential == false) {
				eflags |= ARCHIVE_EXTRACT_NO_OVERWRITE;
				eflags |= ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER;
			}
		}

		/*
		 * Run the pre INSTALL action if the file is there.
		 */
		if (strcmp("./INSTALL", archive_entry_pathname(entry)) == 0) {
			buf = xbps_xasprintf(".%s/metadata/%s/INSTALL",
			    XBPS_META_PATH, pkgname);
			if (buf == NULL)
				return errno;

			actgt = true;
			archive_entry_set_pathname(entry, buf);

			if (archive_read_extract(ar, entry, eflags) != 0) {
				if ((rv = archive_errno(ar)) != EEXIST) {
					free(buf);
					return rv;
				}
			}

			if ((rv = xbps_file_chdir_exec(rootdir, buf, "pre",
			     pkgname, version, NULL)) != 0) {
				free(buf);
				printf("%s: preinst action target error %s\n",
				    pkgname, strerror(errno));
				return rv;
			}
			/* pass to the next entry if successful */
			free(buf);
			continue;

		/*
		 * Unpack metadata files in final directory.
		 */
		} else if (strcmp("./REMOVE",
		    archive_entry_pathname(entry)) == 0) {
			buf2 = xbps_xasprintf(".%s/metadata/%s/REMOVE",
			    XBPS_META_PATH, pkgname);
			if (buf2 == NULL)
				return errno;
			archive_entry_set_pathname(entry, buf2);
			free(buf2);
			buf2 = NULL;
		} else if (strcmp("./files.plist",
		    archive_entry_pathname(entry)) == 0) {
			/*
			 * Now we have a dictionary from the entry
			 * in memory. Will be written to disk later, when
			 * all files are extracted.
			 */
			filesd = xbps_read_dict_from_archive_entry(ar, entry);
			if (filesd == NULL)
				return errno;

			/* Pass to next entry */
			continue;
		} else if (strcmp("./props.plist",
		    archive_entry_pathname(entry)) == 0) {
			buf2 = xbps_xasprintf(".%s/metadata/%s/props.plist",
			    XBPS_META_PATH, pkgname);
			if (buf2 == NULL)
				return errno;
			archive_entry_set_pathname(entry, buf2);
			free(buf2);
		}
		/*
		 * Check if a configuration file is currently installed
		 * and take action. The following actions are executed:
		 *
		 * - if file doesn't exist or new/current are identical:
		 *   install new one.
		 *
		 * - if file exists and new/current do have differences,
		 *   install new file with extension ".new" and preserve
		 *   current file.
		 */
		if ((rv = install_config_file(filesd, entry, eflags)) != 0) {
			prop_object_release(filesd);
			return rv;
		}

		/*
		 * Extract entry from archive.
		 */
		if (archive_read_extract(ar, entry, eflags) != 0) {
			rv = archive_errno(ar);
			if (rv != EEXIST) {
				printf("ERROR: %s...exiting!\n",
				    archive_error_string(ar));
				return rv;;
			} else if (rv == EEXIST) {
				if (flags & XBPS_FLAG_VERBOSE) {
					printf("WARNING: ignoring existent "
					    "path: %s\n",
					    archive_entry_pathname(entry));
				}
				rv = 0;
				continue;
			}
		}
		if (flags & XBPS_FLAG_VERBOSE)
			printf(" %s\n", archive_entry_pathname(entry));
	}

	if ((rv = archive_errno(ar)) == 0) {
		/*
		 * Now that all files were successfully unpacked, we
		 * can safely externalize files.plist because the path
		 * is reachable.
		 */
		buf2 = xbps_xasprintf(".%s/metadata/%s/files.plist",
		    XBPS_META_PATH, pkgname);
		if (!prop_dictionary_externalize_to_file(filesd, buf2)) {
			prop_object_release(filesd);
			free(buf2);
			return errno;
		}
		free(buf2);
	}
	prop_object_release(filesd);

	return rv;
}
