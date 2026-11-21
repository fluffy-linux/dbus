/*
 * SPDX-License-Identifier: MIT
 */

#include <config.h>

#include "bus/config-parser.h"
#include "dbus/dbus-internals.h"
#include "dbus/dbus-list.h"
#include "dbus/dbus-string.h"
#include "dbus/dbus-sysdeps.h"
#include "dbus/dbus-sysdeps-unix.h"
#include "dbus/dbus-test-tap.h"
#include "test/test-utils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
mkdir_p (const char *path)
{
  char *copy;
  char *p;

  copy = strdup (path);
  if (copy == NULL)
    _dbus_test_fatal ("out of memory");

  for (p = copy + 1; *p != '\0'; p++)
    {
      if (*p == '/')
        {
          *p = '\0';
          (void) mkdir (copy, 0755);
          *p = '/';
        }
    }

  (void) mkdir (copy, 0755);
  free (copy);
}

static void
write_text_file (const char *path,
                 const char *contents)
{
  FILE *fp;

  fp = fopen (path, "w");
  if (fp == NULL)
    _dbus_test_fatal ("failed to open %s for writing", path);

  if (fputs (contents, fp) == EOF)
    _dbus_test_fatal ("failed to write %s", path);

  if (fclose (fp) != 0)
    _dbus_test_fatal ("failed to close %s", path);
}

static dbus_bool_t
test_fluffy_glob_config (const char *test_data_dir _DBUS_GNUC_UNUSED)
{
  char root_template[] = "/tmp/dbus-fluffy-test-XXXXXX";
  char *root;
  char orig_cwd[PATH_MAX];
  char cwd_trap[1024];
  char path_buf[2048];
  char conf_xml[8192];
  char pkg_a_sysd[1024];
  char pkg_b_sysd[1024];
  char etc_sysd[1024];
  char pkg_a_svc[1024];
  char pkg_b_svc[1024];
  char main_conf[1024];
  char xdg_glob[1024];
  const char *old_xdg_raw;
  char *old_xdg_copy = NULL;
  DBusString conf_path;
  DBusError error = DBUS_ERROR_INIT;
  BusConfigParser *parser = NULL;
  DBusList **conf_dirs;
  DBusList **service_dirs;
  DBusList *session_dirs = NULL;
  DBusList *link;
  int count;
  dbus_bool_t found_pkg_a = FALSE;
  dbus_bool_t found_pkg_b = FALSE;
  dbus_bool_t ret = FALSE;

  if (getcwd (orig_cwd, sizeof (orig_cwd)) == NULL)
    _dbus_test_fatal ("getcwd failed");

  root = mkdtemp (root_template);
  if (root == NULL)
    _dbus_test_fatal ("mkdtemp failed");

  snprintf (pkg_a_sysd, sizeof (pkg_a_sysd), "%s/pkgs/dbus/share/dbus-1/system.d", root);
  snprintf (pkg_b_sysd, sizeof (pkg_b_sysd), "%s/pkgs/systemd/share/dbus-1/system.d", root);
  snprintf (etc_sysd, sizeof (etc_sysd), "%s/etc/dbus-1/system.d", root);
  snprintf (pkg_a_svc, sizeof (pkg_a_svc), "%s/pkgs/dbus/share/dbus-1/system-services", root);
  snprintf (pkg_b_svc, sizeof (pkg_b_svc), "%s/pkgs/systemd/share/dbus-1/system-services", root);

  mkdir_p (pkg_a_sysd);
  mkdir_p (pkg_b_sysd);
  mkdir_p (etc_sysd);
  mkdir_p (pkg_a_svc);
  mkdir_p (pkg_b_svc);

  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/dbus/share/dbus-1/services", root);
  mkdir_p (path_buf);
  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/systemd/share/dbus-1/services", root);
  mkdir_p (path_buf);
  /* Package without dbus-1 subdir and regular file matching glob must be ignored */
  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/bash/share/doc", root);
  mkdir_p (path_buf);
  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/notadir/share/dbus-1", root);
  mkdir_p (path_buf);
  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/notadir/share/dbus-1/services", root);
  write_text_file (path_buf, "not a directory\n");

  /* Symlinked aliases to existing <includedir> / <servicedir> must deduplicate by (st_dev, st_ino) */
  snprintf (path_buf, sizeof (path_buf), "%s/alias-system.d", root);
  if (symlink (pkg_a_sysd, path_buf) != 0)
    _dbus_test_fatal ("symlink alias-system.d failed");
  snprintf (path_buf, sizeof (path_buf), "%s/alias-system-services", root);
  if (symlink (pkg_a_svc, path_buf) != 0)
    _dbus_test_fatal ("symlink alias-system-services failed");

  snprintf (path_buf, sizeof (path_buf), "%s/00-dbus.conf", pkg_a_sysd);
  write_text_file (path_buf,
                   "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig>\n"
                   "  <pidfile>/run/pkg-a.pid</pidfile>\n"
                   "</busconfig>\n");

  snprintf (path_buf, sizeof (path_buf), "%s/org.freedesktop.login1.conf", pkg_b_sysd);
  write_text_file (path_buf,
                   "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig>\n"
                   "  <pidfile>/run/systemd-login1.pid</pidfile>\n"
                   "  <policy user=\"root\">\n"
                   "    <allow own=\"org.freedesktop.login1\"/>\n"
                   "  </policy>\n"
                   "</busconfig>\n");

  snprintf (main_conf, sizeof (main_conf), "%s/pkgs/dbus/share/dbus-1/system.conf", root);
  snprintf (conf_xml, sizeof (conf_xml),
            "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
            " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
            "<busconfig>\n"
            "  <type>system</type>\n"
            "  <listen>unix:path=%s/system_bus_socket</listen>\n"
            "  <servicedir>%s/pkgs/*/share/dbus-1/system-services</servicedir>\n"
            "  <servicedir>%s/alias-system-services</servicedir>\n"
            "  <servicedir>%s/pkgs/*/nonexistent-services</servicedir>\n"
            "  <includedir>system.d</includedir>\n"
            "  <includedir>%s/pkgs/*/share/dbus-1/system.d</includedir>\n"
            "  <includedir>%s/alias-system.d</includedir>\n"
            "  <includedir>%s/pkgs/*/etc/dbus-1/system.d</includedir>\n"
            "  <includedir>%s/etc/dbus-1/system.d</includedir>\n"
            "</busconfig>\n",
            root, root, root, root, root, root, root, root);
  write_text_file (main_conf, conf_xml);

  _dbus_string_init_const (&conf_path, main_conf);
  parser = bus_config_load (&conf_path, TRUE, NULL, &error);
  if (parser == NULL)
    _dbus_test_fatal ("bus_config_load failed: %s", error.message);

  /* system.d from basedir and from wildcard system.d paths must be deduplicated,
   * and alphabetical glob order means pkgs/systemd overrides pkgs/dbus pidfile */
  if (bus_config_parser_get_pidfile (parser) == NULL ||
      strcmp (bus_config_parser_get_pidfile (parser), "/run/systemd-login1.pid") != 0)
    {
      _dbus_warn ("unexpected pidfile: %s",
                  bus_config_parser_get_pidfile (parser) ?
                  bus_config_parser_get_pidfile (parser) : "(null)");
      goto out;
    }

  conf_dirs = bus_config_parser_get_conf_dirs (parser);
  count = 0;
  for (link = _dbus_list_get_first_link (conf_dirs);
       link != NULL;
       link = _dbus_list_get_next_link (conf_dirs, link))
    {
      const char *d = link->data;

      if (strcmp (d, pkg_a_sysd) == 0)
        found_pkg_a = TRUE;
      else if (strcmp (d, pkg_b_sysd) == 0)
        found_pkg_b = TRUE;
      count++;
    }

  /* 3 conf_dirs: pkgs/dbus, pkgs/systemd, and etc/dbus-1/system.d */
  if (count != 3 || !found_pkg_a || !found_pkg_b)
    {
      _dbus_warn ("expected 3 deduplicated conf_dirs, got %d", count);
      goto out;
    }

  found_pkg_a = FALSE;
  found_pkg_b = FALSE;
  count = 0;
  service_dirs = bus_config_parser_get_service_dirs (parser);
  for (link = _dbus_list_get_first_link (service_dirs);
       link != NULL;
       link = _dbus_list_get_next_link (service_dirs, link))
    {
      BusConfigServiceDir *sd = link->data;

      if (strcmp (sd->path, pkg_a_svc) == 0)
        found_pkg_a = TRUE;
      else if (strcmp (sd->path, pkg_b_svc) == 0)
        found_pkg_b = TRUE;
      count++;
    }

  if (count != 2 || !found_pkg_a || !found_pkg_b)
    {
      _dbus_warn ("expected exactly 2 globbed service_dirs, got %d", count);
      goto out;
    }

  /* Verify /etc/dbus-1/system.d overrides all vendor packages */
  bus_config_parser_unref (parser);
  parser = NULL;
  snprintf (path_buf, sizeof (path_buf), "%s/99-local.conf", etc_sysd);
  write_text_file (path_buf,
                   "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig>\n"
                   "  <pidfile>/run/etc-override.pid</pidfile>\n"
                   "</busconfig>\n");

  parser = bus_config_load (&conf_path, TRUE, NULL, &error);
  if (parser == NULL)
    _dbus_test_fatal ("bus_config_load with /etc override failed: %s", error.message);

  if (bus_config_parser_get_pidfile (parser) == NULL ||
      strcmp (bus_config_parser_get_pidfile (parser), "/run/etc-override.pid") != 0)
    {
      _dbus_warn ("expected /etc override pidfile, got %s",
                  bus_config_parser_get_pidfile (parser) ?
                  bus_config_parser_get_pidfile (parser) : "(null)");
      goto out;
    }

  /* Verify _dbus_split_glob_paths_and_append via XDG_DATA_DIRS wildcard,
   * including empty ':' segments (which XDG requires skipping, NEVER mapping to '.'),
   * '[[:alpha:]]' POSIX classes, '[a:]' bracketed colons, and unclosed '[' segments */
  snprintf (cwd_trap, sizeof (cwd_trap), "%s/cwd_trap", root);
  snprintf (path_buf, sizeof (path_buf), "%s/dbus-1/services", cwd_trap);
  mkdir_p (path_buf);
  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/1:colon/share/dbus-1/services", root);
  mkdir_p (path_buf);

  old_xdg_raw = getenv ("XDG_DATA_DIRS");
  if (old_xdg_raw != NULL)
    old_xdg_copy = strdup (old_xdg_raw);

  if (chdir (cwd_trap) != 0)
    _dbus_test_fatal ("chdir cwd_trap failed");

  snprintf (xdg_glob, sizeof (xdg_glob),
            ":%s/[unclosed/sub:%s/pkgs/[[:alpha:]]*/share:%s/pkgs/1[a:]colon/share::",
            root, root, root);
  setenv ("XDG_DATA_DIRS", xdg_glob, 1);

  if (!_dbus_get_standard_session_servicedirs (&session_dirs))
    _dbus_test_fatal ("_dbus_get_standard_session_servicedirs failed");

  if (chdir (orig_cwd) != 0)
    _dbus_test_fatal ("chdir orig_cwd failed");

  if (old_xdg_copy != NULL)
    {
      setenv ("XDG_DATA_DIRS", old_xdg_copy, 1);
      free (old_xdg_copy);
      old_xdg_copy = NULL;
    }
  else
    {
      unsetenv ("XDG_DATA_DIRS");
    }

  found_pkg_a = FALSE;
  found_pkg_b = FALSE;
  count = 0;
  snprintf (pkg_a_svc, sizeof (pkg_a_svc), "%s/pkgs/dbus/share/dbus-1/services", root);
  snprintf (pkg_b_svc, sizeof (pkg_b_svc), "%s/pkgs/systemd/share/dbus-1/services", root);
  snprintf (path_buf, sizeof (path_buf), "%s/pkgs/1:colon/share/dbus-1/services", root);

  for (link = _dbus_list_get_first_link (&session_dirs);
       link != NULL;
       link = _dbus_list_get_next_link (&session_dirs, link))
    {
      const char *d = link->data;

      if (strcmp (d, pkg_a_svc) == 0)
        found_pkg_a = TRUE;
      else if (strcmp (d, pkg_b_svc) == 0)
        found_pkg_b = TRUE;
      else if (strcmp (d, path_buf) == 0)
        count++;
      else if (strstr (d, "/pkgs/bash/") != NULL ||
               strstr (d, "/pkgs/notadir/") != NULL ||
               strstr (d, "cwd_trap") != NULL ||
               strcmp (d, "dbus-1/services") == 0 ||
               strcmp (d, "./dbus-1/services") == 0 ||
               strcmp (d, "/dbus-1/services") == 0)
        {
          _dbus_warn ("unexpected session service dir %s", d);
          goto out;
        }
    }

  if (!found_pkg_a || !found_pkg_b || count != 1)
    {
      _dbus_warn ("missing globbed session service dirs");
      goto out;
    }

  ret = TRUE;

out:
  if (chdir (orig_cwd) != 0)
    _dbus_test_fatal ("chdir orig_cwd failed");
  _dbus_list_clear_full (&session_dirs, dbus_free);
  if (parser != NULL)
    bus_config_parser_unref (parser);
  dbus_error_free (&error);
  free (old_xdg_copy);
  return ret;
}

static const DBusTestCase tests[] =
{
  { "fluffy_glob_config", test_fluffy_glob_config },
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  return _dbus_test_main (argc, argv, _DBUS_N_ELEMENTS (tests), tests,
                          DBUS_TEST_FLAGS_NONE,
                          NULL, NULL);
}
