/* -*- c-file-style: "gnu" -*-
 * Switch to new root directory and start init.
 *
 * Copyright 2011,2012,2013 Colin Walters <walters@verbum.org>
 *
 * Based on code from util-linux/sys-utils/switch_root.c,
 * Copyright 2002-2009 Red Hat, Inc.  All rights reserved.
 * Authors:
 *  Peter Jones <pjones@redhat.com>
 *  Jeremy Katz <katzj@redhat.com>
 *
 * Relicensed with permission to LGPLv2+.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

/* The high level goal of ostree-prepare-root.service is to run inside
 * the initial ram disk (if one is in use) and set up the `/` mountpoint
 * to be the deployment root, using the ostree= kernel commandline
 * argument to find the target deployment root.
 *
 * It's really the heart of how ostree works - basically multiple
 * hardlinked chroot() targets are maintained, this one does the equivalent
 * of chroot().
 *
 * # ostree-prepare-root.service
 *
 * If using systemd, an excellent reference is `man bootup`.  This
 * service runs Before=initrd-root-fs.target.  At this point it's
 * assumed that the block storage and root filesystem are mounted at
 * /sysroot - i.e. /sysroot points to the *physical* root before
 * this service runs.  After, `/` is the deployment root, and /sysroot is
 * the physical root.
 *
 * # Running as pid 1
 *
 * See ostree-prepare-root-static.c for this.
 */

#include "config.h"

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libglnx.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <ostree-core.h>
#include <ostree-repo-private.h>

#include "otcore.h"

#define OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG \
  SD_ID128_MAKE (71, 70, 33, 6a, 73, ba, 46, 01, ba, d3, 1a, f8, 88, aa, 0d, f7)

// A temporary mount point
#define TMP_SYSROOT "/sysroot.tmp"

#ifdef HAVE_COMPOSEFS
#include <libcomposefs/lcfs-mount.h>
#include <libcomposefs/lcfs-writer.h>
#endif

#include "ostree-mount-util.h"

typedef enum
{
  OSTREE_COMPOSEFS_MODE_OFF,    /* Never use composefs */
  OSTREE_COMPOSEFS_MODE_MAYBE,  /* Use if supported and image exists in deploy */
  OSTREE_COMPOSEFS_MODE_ON,     /* Always use (and fail if not working) */
  OSTREE_COMPOSEFS_MODE_SIGNED, /* Always use and require it to be signed */
  OSTREE_COMPOSEFS_MODE_DIGEST, /* Always use and require specific digest */
} OstreeComposefsMode;

static bool
sysroot_is_configured_ro (const char *sysroot)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree char *repo_config_path = g_build_filename (sysroot, "ostree/repo/config", NULL);
  g_autoptr (GKeyFile) repo_config = g_key_file_new ();
  if (!g_key_file_load_from_file (repo_config, repo_config_path, G_KEY_FILE_NONE, &local_error))
    {
      g_printerr ("Failed to load %s: %s", repo_config_path, local_error->message);
      return false;
    }

  return g_key_file_get_boolean (repo_config, "sysroot", "readonly", NULL);
}

static char *
resolve_deploy_path (const char *root_mountpoint)
{
  char destpath[PATH_MAX];
  struct stat stbuf;
  char *deploy_path;
  autofree char *ostree_target = get_ostree_target ();
  if (!ostree_target)
    errx (EXIT_FAILURE, "No ostree= cmdline");

  if (snprintf (destpath, sizeof (destpath), "%s/%s", root_mountpoint, ostree_target) < 0)
    err (EXIT_FAILURE, "failed to assemble ostree target path");
  if (lstat (destpath, &stbuf) < 0)
    err (EXIT_FAILURE, "Couldn't find specified OSTree root '%s'", destpath);
  if (!S_ISLNK (stbuf.st_mode))
    errx (EXIT_FAILURE, "OSTree target is not a symbolic link: %s", destpath);
  deploy_path = realpath (destpath, NULL);
  if (deploy_path == NULL)
    err (EXIT_FAILURE, "realpath(%s) failed", destpath);
  if (stat (deploy_path, &stbuf) < 0)
    err (EXIT_FAILURE, "stat(%s) failed", deploy_path);
  /* Quiet logs if there's no journal */
  const char *resolved_path = deploy_path + strlen (root_mountpoint);
  ot_journal_send ("MESSAGE=Resolved OSTree target to: %s", deploy_path,
                   "MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL (OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG), "DEPLOYMENT_PATH=%s",
                   resolved_path, "DEPLOYMENT_DEVICE=%" PRIu64, (uint64_t)stbuf.st_dev,
                   "DEPLOYMENT_INODE=%" PRIu64, (uint64_t)stbuf.st_ino, NULL);
  return deploy_path;
}

static int
pivot_root (const char *new_root, const char *put_old)
{
  return syscall (__NR_pivot_root, new_root, put_old);
}

#ifdef HAVE_COMPOSEFS
static GVariant *
load_variant (const char *root_mountpoint, const char *digest, const char *extension,
              const GVariantType *type, GError **error)
{
  g_autofree char *path = NULL;
  char *data = NULL;
  gsize data_size;

  path = g_strdup_printf ("%s/ostree/repo/objects/%.2s/%s.%s", root_mountpoint, digest, digest + 2,
                          extension);

  if (!g_file_get_contents (path, &data, &data_size, error))
    return NULL;

  return g_variant_ref_sink (g_variant_new_from_data (type, data, data_size, FALSE, g_free, data));
}

static gboolean
load_commit_for_deploy (const char *root_mountpoint, const char *deploy_path, GVariant **commit_out,
                        GVariant **commitmeta_out, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree char *digest = g_path_get_basename (deploy_path);
  char *dot;

  dot = strchr (digest, '.');
  if (dot != NULL)
    *dot = 0;

  g_autoptr (GVariant) commit_v
      = load_variant (root_mountpoint, digest, "commit", OSTREE_COMMIT_GVARIANT_FORMAT, error);
  if (commit_v == NULL)
    return FALSE;

  g_autoptr (GVariant) commitmeta_v = load_variant (root_mountpoint, digest, "commitmeta",
                                                    G_VARIANT_TYPE ("a{sv}"), &local_error);
  if (commitmeta_v == NULL)
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        glnx_throw (error, "No commitmeta for commit %s", digest);
      else
        g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  *commit_out = g_steal_pointer (&commit_v);
  *commitmeta_out = g_steal_pointer (&commitmeta_v);

  return TRUE;
}

static gboolean
validate_signature (GBytes *data, GVariant *signatures, const guchar *pubkey, size_t pubkey_size)
{
  g_autoptr (GBytes) pubkey_buf = g_bytes_new_static (pubkey, pubkey_size);

  for (gsize i = 0; i < g_variant_n_children (signatures); i++)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
      g_autoptr (GBytes) signature = g_variant_get_data_as_bytes (child);
      bool valid = false;

      if (!otcore_validate_ed25519_signature (data, pubkey_buf, signature, &valid, &local_error))
        errx (EXIT_FAILURE, "signature verification failed: %s", local_error->message);
      if (valid)
        return TRUE;
    }

  return FALSE;
}
#endif

int
main (int argc, char *argv[])
{
  char srcpath[PATH_MAX];

  const char *root_arg = NULL;
  bool we_mounted_proc = false;
  g_autoptr (GError) error = NULL;

  if (argc < 2)
    err (EXIT_FAILURE, "usage: ostree-prepare-root SYSROOT");
  root_arg = argv[1];

  struct stat stbuf;
  if (stat ("/proc/cmdline", &stbuf) < 0)
    {
      if (errno != ENOENT)
        err (EXIT_FAILURE, "stat(\"/proc/cmdline\") failed");
      /* We need /proc mounted for /proc/cmdline and realpath (on musl) to
       * work: */
      if (mount ("proc", "/proc", "proc", MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to mount proc on /proc");
      we_mounted_proc = 1;
    }

  /* This is the final target where we should prepare the rootfs.  The usual
   * case with systemd in the initramfs is that root_mountpoint = "/sysroot".
   * In the fastboot embedded case we're pid1 and will setup / ourself, and
   * then root_mountpoint = "/".
   * */
  const char *root_mountpoint = realpath (root_arg, NULL);
  if (root_mountpoint == NULL)
    err (EXIT_FAILURE, "realpath(\"%s\")", root_arg);
  char *deploy_path = resolve_deploy_path (root_mountpoint);

  if (we_mounted_proc)
    {
      /* Leave the filesystem in the state that we found it: */
      if (umount ("/proc"))
        err (EXIT_FAILURE, "failed to umount proc from /proc");
    }

  OstreeComposefsMode composefs_mode = OSTREE_COMPOSEFS_MODE_MAYBE;
  autofree char *ot_composefs = read_proc_cmdline_key ("ot-composefs");
  char *composefs_digest = NULL;
  char *composefs_pubkey = NULL;
  if (ot_composefs)
    {
      if (strcmp (ot_composefs, "off") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_OFF;
      else if (strcmp (ot_composefs, "maybe") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_MAYBE;
      else if (strcmp (ot_composefs, "on") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_ON;
      else if (strncmp (ot_composefs, "signed=", strlen ("signed=")) == 0)
        {
          composefs_mode = OSTREE_COMPOSEFS_MODE_SIGNED;
          composefs_pubkey = ot_composefs + strlen ("signed=");
        }
      else if (strncmp (ot_composefs, "digest=", strlen ("digest=")) == 0)
        {
          composefs_mode = OSTREE_COMPOSEFS_MODE_DIGEST;
          composefs_digest = ot_composefs + strlen ("digest=");
        }
      else
        err (EXIT_FAILURE, "Unsupported ot-composefs option: '%s'", ot_composefs);
    }

#ifndef HAVE_COMPOSEFS
  if (composefs_mode == OSTREE_COMPOSEFS_MODE_MAYBE)
    composefs_mode = OSTREE_COMPOSEFS_MODE_OFF;
  (void)composefs_digest;
  (void)composefs_pubkey;
#endif

  /* Query the repository configuration - this is an operating system builder
   * choice.  More info: https://github.com/ostreedev/ostree/pull/1767
   */
  const bool sysroot_readonly = sysroot_is_configured_ro (root_arg);
  const bool sysroot_currently_writable = !path_is_on_readonly_fs (root_arg);
  g_print ("sysroot.readonly configuration value: %d (fs writable: %d)\n", (int)sysroot_readonly,
           (int)sysroot_currently_writable);

  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount (NULL, "/", NULL, MS_REC | MS_PRIVATE | MS_SILENT, NULL) < 0)
    err (EXIT_FAILURE, "failed to make \"/\" private mount");

  if (mkdir (TMP_SYSROOT, 0755) < 0)
    err (EXIT_FAILURE, "couldn't create temporary sysroot %s", TMP_SYSROOT);

  /* Run in the deploy_path dir so we can use relative paths below */
  if (chdir (deploy_path) < 0)
    err (EXIT_FAILURE, "failed to chdir to deploy_path");

  GVariantBuilder metadata_builder;
  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));
  bool using_composefs = false;

  /* We construct the new sysroot in /sysroot.tmp, which is either the composfs
     mount or a bind mount of the deploy-dir */
  if (composefs_mode != OSTREE_COMPOSEFS_MODE_OFF)
    {
#ifdef HAVE_COMPOSEFS
      const char *objdirs[] = { "/sysroot/ostree/repo/objects" };
      g_autofree char *cfs_digest = NULL;
      struct lcfs_mount_options_s cfs_options = {
        objdirs,
        1,
      };

      if (composefs_mode == OSTREE_COMPOSEFS_MODE_SIGNED)
        {
          g_autoptr (GError) local_error = NULL;
          g_autofree char *pubkey = NULL;
          gsize pubkey_size;
          g_autoptr (GVariant) commit = NULL;
          g_autoptr (GVariant) commitmeta = NULL;

          if (!g_file_get_contents (composefs_pubkey, &pubkey, &pubkey_size, &local_error))
            errx (EXIT_FAILURE, "Failed to load public key '%s': %s", composefs_pubkey,
                  local_error->message);

          if (!load_commit_for_deploy (root_mountpoint, deploy_path, &commit, &commitmeta,
                                       &local_error))
            errx (EXIT_FAILURE, "Error loading signatures from repo: %s", local_error->message);

          g_autoptr (GVariant) signatures = g_variant_lookup_value (
              commitmeta, OSTREE_SIGN_METADATA_ED25519_KEY, G_VARIANT_TYPE ("aay"));
          if (signatures == NULL)
            errx (EXIT_FAILURE, "Signature validation requested, but no signatures in commit");

          g_autoptr (GBytes) commit_data = g_variant_get_data_as_bytes (commit);
          if (!validate_signature (commit_data, signatures, (guchar *)pubkey, pubkey_size))
            errx (EXIT_FAILURE, "No valid signatures found for public key");

          g_print ("Validated commit signature using '%s'\n", composefs_pubkey);
          g_variant_builder_add (&metadata_builder, "{sv}",
                                 OTCORE_RUN_BOOTED_KEY_COMPOSEFS_SIGNATURE,
                                 g_variant_new_string (composefs_pubkey));

          g_autoptr (GVariant) metadata = g_variant_get_child_value (commit, 0);
          g_autoptr (GVariant) cfs_digest_v = g_variant_lookup_value (
              metadata, OSTREE_COMPOSEFS_DIGEST_KEY_V0, G_VARIANT_TYPE_BYTESTRING);
          if (cfs_digest_v == NULL || g_variant_get_size (cfs_digest_v) != OSTREE_SHA256_DIGEST_LEN)
            errx (EXIT_FAILURE, "Signature validation requested, but no valid digest in commit");

          composefs_digest = g_malloc (OSTREE_SHA256_STRING_LEN + 1);
          ot_bin2hex (composefs_digest, g_variant_get_data (cfs_digest_v),
                      g_variant_get_size (cfs_digest_v));
        }

      cfs_options.flags = LCFS_MOUNT_FLAGS_READONLY;

      if (snprintf (srcpath, sizeof (srcpath), "%s/.ostree.mnt", deploy_path) < 0)
        err (EXIT_FAILURE, "failed to assemble /boot/loader path");
      cfs_options.image_mountdir = srcpath;

      if (composefs_digest != NULL)
        {
          cfs_options.flags |= LCFS_MOUNT_FLAGS_REQUIRE_VERITY;
          cfs_options.expected_fsverity_digest = composefs_digest;
        }

      if (composefs_mode == OSTREE_COMPOSEFS_MODE_MAYBE)
        g_print ("Trying to mount composefs rootfs\n");
      else if (composefs_digest != NULL)
        g_print ("Mounting composefs rootfs with expected digest '%s'\n", composefs_digest);
      else
        g_print ("Mounting composefs rootfs\n");

      if (lcfs_mount_image (OSTREE_COMPOSEFS_NAME, TMP_SYSROOT, &cfs_options) == 0)
        {
          using_composefs = 1;
          g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_COMPOSEFS,
                                 g_variant_new_boolean (true));
        }
      else
        {
          if (errno == ENOVERITY)
            g_print ("No verity in composefs image\n");
          else if (errno == EWRONGVERITY)
            g_print ("Wrong verity digest in composefs image\n");
          else if (errno == ENOSIGNATURE)
            g_print ("Missing signature in composefs image\n");
          else
            g_print ("Mounting composefs image failed: %s\n", strerror (errno));
        }
#else
      err (EXIT_FAILURE, "Composefs not supported");
#endif
    }

  if (!using_composefs)
    {
      if (composefs_mode > OSTREE_COMPOSEFS_MODE_MAYBE)
        err (EXIT_FAILURE, "Failed to mount composefs");

      /* The deploy root starts out bind mounted to sysroot.tmp */
      if (mount (deploy_path, TMP_SYSROOT, NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to make initial bind mount %s", deploy_path);
    }
  else
    {
      g_print ("Mounted composefs\n");
    }

  /* This will result in a system with /sysroot read-only. Thus, two additional
   * writable bind-mounts (for /etc and /var) are required later on. */
  if (sysroot_readonly)
    {
      if (!sysroot_currently_writable)
        errx (EXIT_FAILURE, "sysroot.readonly=true requires %s to be writable at this point",
              root_arg);
    }
  /* Pass on the state for use by ostree-prepare-root */
  g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_SYSROOT_RO,
                         g_variant_new_boolean (sysroot_readonly));

  /* Prepare /boot.
   * If /boot is on the same partition, use a bind mount to make it visible
   * at /boot inside the deployment. */
  if (snprintf (srcpath, sizeof (srcpath), "%s/boot/loader", root_mountpoint) < 0)
    err (EXIT_FAILURE, "failed to assemble /boot/loader path");
  if (lstat (srcpath, &stbuf) == 0 && S_ISLNK (stbuf.st_mode))
    {
      if (lstat ("boot", &stbuf) == 0 && S_ISDIR (stbuf.st_mode))
        {
          if (snprintf (srcpath, sizeof (srcpath), "%s/boot", root_mountpoint) < 0)
            err (EXIT_FAILURE, "failed to assemble /boot path");
          if (mount (srcpath, TMP_SYSROOT "/boot", NULL, MS_BIND | MS_SILENT, NULL) < 0)
            err (EXIT_FAILURE, "failed to bind mount %s to boot", srcpath);
        }
    }

  /* Prepare /etc.
   * No action required if sysroot is writable. Otherwise, a bind-mount for
   * the deployment needs to be created and remounted as read/write. */
  if (sysroot_readonly || using_composefs)
    {
      /* Bind-mount /etc (at deploy path), and remount as writable. */
      if (mount ("etc", TMP_SYSROOT "/etc", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to prepare /etc bind-mount at /sysroot.tmp/etc");
      if (mount (TMP_SYSROOT "/etc", TMP_SYSROOT "/etc", NULL, MS_BIND | MS_REMOUNT | MS_SILENT,
                 NULL)
          < 0)
        err (EXIT_FAILURE, "failed to make writable /etc bind-mount at /sysroot.tmp/etc");
    }

  /* Prepare /usr.
   * It may be either just a read-only bind-mount, or a persistent overlayfs. */
  if (lstat (".usr-ovl-work", &stbuf) == 0)
    {
      /* Do we have a persistent overlayfs for /usr?  If so, mount it now. */
      const char usr_ovl_options[]
          = "lowerdir=" TMP_SYSROOT "/usr,upperdir=.usr-ovl-upper,workdir=.usr-ovl-work";

      /* Except overlayfs barfs if we try to mount it on a read-only
       * filesystem.  For this use case I think admins are going to be
       * okay if we remount the rootfs here, rather than waiting until
       * later boot and `systemd-remount-fs.service`.
       */
      if (path_is_on_readonly_fs (TMP_SYSROOT))
        {
          if (mount (TMP_SYSROOT, TMP_SYSROOT, NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
            err (EXIT_FAILURE, "failed to remount rootfs writable (for overlayfs)");
        }

      if (mount ("overlay", TMP_SYSROOT "/usr", "overlay", MS_SILENT, usr_ovl_options) < 0)
        err (EXIT_FAILURE, "failed to mount /usr overlayfs");
    }
  else if (!using_composefs)
    {
      /* Otherwise, a read-only bind mount for /usr. (Not needed for composefs) */
      if (mount (TMP_SYSROOT "/usr", TMP_SYSROOT "/usr", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
      if (mount (TMP_SYSROOT "/usr", TMP_SYSROOT "/usr", NULL,
                 MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL)
          < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
    }

  /* Prepare /var.
   * When a read-only sysroot is configured, this adds a dedicated bind-mount (to itself)
   * so that the stateroot location stays writable. */
  if (sysroot_readonly)
    {
      /* Bind-mount /var (at stateroot path), and remount as writable. */
      if (mount ("../../var", "../../var", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to prepare /var bind-mount at %s", srcpath);
      if (mount ("../../var", "../../var", NULL, MS_BIND | MS_REMOUNT | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to make writable /var bind-mount at %s", srcpath);
    }

    /* When running under systemd, /var will be handled by a 'var.mount' unit outside
     * of initramfs.
     * Systemd auto-detection can be overridden by a marker file under /run. */
#ifdef HAVE_SYSTEMD_AND_LIBMOUNT
  bool mount_var = false;
#else
  bool mount_var = true;
#endif
  if (lstat (INITRAMFS_MOUNT_VAR, &stbuf) == 0)
    mount_var = true;

  /* If required, bind-mount `/var` in the deployment to the "stateroot", which is
   *  the shared persistent directory for a set of deployments.  More info:
   *  https://ostreedev.github.io/ostree/deployment/#stateroot-aka-osname-group-of-deployments-that-share-var
   */
  if (mount_var)
    {
      if (mount ("../../var", TMP_SYSROOT "/var", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount ../../var to var");
    }

  /* This can be used by other things to signal ostree is in use */
  {
    g_autoptr (GVariant) metadata = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));
    const guint8 *buf = g_variant_get_data (metadata) ?: (guint8 *)"";
    if (!glnx_file_replace_contents_at (AT_FDCWD, OTCORE_RUN_BOOTED, buf,
                                        g_variant_get_size (metadata), 0, NULL, &error))
      errx (EXIT_FAILURE, "Writing %s: %s", OTCORE_RUN_BOOTED, error->message);
  }

  if (chdir (TMP_SYSROOT) < 0)
    err (EXIT_FAILURE, "failed to chdir to " TMP_SYSROOT);

  if (strcmp (root_mountpoint, "/") == 0)
    {
      /* pivot_root rotates two mount points around.  In this instance . (the
       * deploy location) becomes / and the existing / becomes /sysroot.  We
       * have to use pivot_root rather than mount --move in this instance
       * because our deploy location is mounted as a subdirectory of the real
       * sysroot, so moving sysroot would also move the deploy location.   In
       * reality attempting mount --move would fail with EBUSY. */
      if (pivot_root (".", "sysroot") < 0)
        err (EXIT_FAILURE, "failed to pivot_root to deployment");
    }
  else
    {
      /* In this instance typically we have our ready made-up up root at
       * /sysroot.tmp and the physical root at /sysroot (root_mountpoint).
       * We want to end up with our deploy root at /sysroot/ and the physical
       * root under /sysroot/sysroot as systemd will be responsible for
       * moving /sysroot to /.
       */
      if (mount (root_mountpoint, "sysroot", NULL, MS_MOVE | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE '%s' to 'sysroot'", root_mountpoint);

      if (mount (".", root_mountpoint, NULL, MS_MOVE | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE %s to %s", ".", root_mountpoint);

      if (chdir (root_mountpoint) < 0)
        err (EXIT_FAILURE, "failed to chdir to %s", root_mountpoint);

      if (rmdir (TMP_SYSROOT) < 0)
        err (EXIT_FAILURE, "couldn't remove temporary sysroot %s", TMP_SYSROOT);

      if (sysroot_readonly)
        {
          if (mount ("sysroot", "sysroot", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL)
              < 0)
            err (EXIT_FAILURE, "failed to make /sysroot read-only");

          /* TODO(lucab): This will make the final '/' read-only.
           * Stabilize read-only '/sysroot' first, then enable this additional hardening too.
           *
           * if (mount (".", ".", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL) < 0)
           *   err (EXIT_FAILURE, "failed to make / read-only");
           */
        }
    }

  /* The /sysroot mount needs to be private to avoid having a mount for e.g. /var/cache
   * also propagate to /sysroot/ostree/deploy/$stateroot/var/cache
   *
   * Now in reality, today this is overridden by systemd: the *actual* way we fix this up
   * is in ostree-remount.c.  But let's do it here to express the semantics we want
   * at the very start (perhaps down the line systemd will have compile/runtime option
   * to say that the initramfs environment did everything right from the start).
   */
  if (mount ("none", "sysroot", NULL, MS_PRIVATE | MS_SILENT, NULL) < 0)
    err (EXIT_FAILURE, "remounting 'sysroot' private");

  exit (EXIT_SUCCESS);
}
