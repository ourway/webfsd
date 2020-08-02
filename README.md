
This is a simple http server for pure static content.  You
can use it to serve the content of a ftp server via http for
example.  It is also nice to export some files the quick way
by starting a http server in a few seconds, without editing
some config file first.

It uses sendfile() and knows how to use sendfile on linux and FreeBSD.
Adding other systems shouldn't be difficult.  To use it with linux
you'll need a 2.2.x kernel and glibc 2.1.

There is some sendfile emulation code which uses a userland bounce
buffer, this allows to compile and use webfs on systems without
sendfile().


Features/Design:
================

 * single process: select() + non-blocking I/O.
 * trimmed to use as few system calls as possible per request.
 * use sendfile to avoid copying data to userspace.
 * optional thread support.  Every thread has its own select
   loop then (compile time option, off by default, edit the
   Makefile to turn it on).
 * automatically generates directory listings when asked for a
   directory (check for index.html available as option), caches
   the listings.
 * no config file, just a few switches.  Try "webfsd -h" for a
   list, check the man page for a more indepth description.
 * Uses /etc/mime.types to map file extentions to mime/types.
 * Uses normal unix access rights, it will deliver every regular
   file it is able to open for reading.  If you want it to serve
   public-readable files only, make sure it runs as nobody/nogroup.
 * supports keep-alive and pipelined requests.
 * serves byte ranges.
 * supports virtual hosts.
 * supports ipv6.
 * optional logging in common log file format.
 * optional error logging (to syslog / stderr).
 * limited CGI support (GET requests only).
 * optional SSL support.


Plans/BUGS/TODO
===============

 * figure out why the acroread plugin doesn't like my
   multipart/byteranges responses.
 * benchmarking / profiling.

Don't expect much more features.  I want to keep it small and
simple. It is supported to serve just files and to do this in a good
and fast way.  It is supposed to be HTTP/1.1 (RfC 2068) compliant.
Conditional compliant as there is no entity tag support.


Compile/Install
===============

$ make
$ su -c "make install"

See INSTALL for more details.


Tuning
======

The default for the number of parallel connections is very low (32),
you might have to raise this.

You probably don't get better performance by turning on threads.  For
static content I/O bandwidth is the bottleneck.  My box easily fills
up the network bandwidth while webfsd uses less than 10% CPU time
(Pentium III/450 MHz, Fast Ethernet, Tulip card).

You might win with threads if you have a very fast network connection
and a lot of traffic.  The sendfile() system call blocks if it has to
read from harddisk.  While one thread waits for data in sendfile(),
another can keep the network card busy.  You'll probably get best
results with a small number of threads (2-3) per CPU.

Enough RAM probably also helps to speed up things.  Although webfs
itself will not need very much memory, your kernel will happily use
the memory as cache for the data sent out via sendfile().

I have no benchmark numbers for webfsd.


Security
========

I can't guarantee that there are no security flaws.  If you find one,
report it as a bug.  I've done my very best while writing webfsd, I hope
there are no serious bugs like buffer overflows (and no other bugs of
course...).  If webfsd dumps core, you /have/ a problem; this really
shouldn't happen.

Don't use versions below 1.20, there are known security holes.


Changes in 1.21
===============

  * large file support.
  * s/sprintf/snprintf/ in some places.
  * changed timestamp handling, webfs doesn't attempt to parse them
    any more but does a strcmp of rfc1123 dates.
  * access log uses local time not GMT now.
  * some ssl/cgi cleanups (based on patches from Ludo Stellingwerff).
  * misc fixes.


Changes in 1.20
===============

  * CGI pipe setup bugfix.
  * Don't allow ".." as hostname (security hole with vhosts enabled).
  * fix buffer overflow in ls.c with very long file names.
  * misc other fixes / cleanups.


Changes in 1.19
===============

  * documentation spell fixes (Ludo Stellingwerff).
  * added missing items (last two) to the 1.18 Changes notes
    (pointed out by Jedi/Sector One <j@pureftpd.org>).
  * Makefile changes.
  * finished user home-directory support.


Changes in 1.18
===============

  * added -j switch.
  * compile fixes for the threaded version.
  * use accept filters (FreeBSD).
  * shuffled around access log locks.
  * added optional SSL support (based on patches by
    Ludo Stellingwerff <ludo@jonkers.nl>).
  * run only the absolute needed code with root privileges
    (bind+chroot) if installed suid-root.
  * Makefile tweaks.
  * fixed buffer overflow in request.c
  * started user home-directory support.


Changes in 1.17
===============

  * fix bug in request cleanup code (didn't cleanup properly after
    byte-range requests, thus making webfsd bomb out on non-range
    requests following a byte-range request on the same keep-alive
    connection).


Changes in 1.16
===============

  * fix bug in %xx handling (adding CGI support broke this).


Changes in 1.14
===============

  * allways use Host: supplied hostname if needed (redirect, ...).
  * added -4 / -6 switches.
  * Added CGI support (GET requests only).
  * compile fix for OpenBSD


Changes in 1.13
===============

  * fixed a bug in Basic authentication.


Changes in 1.11
===============

  * bumped the version number this time :-)
  * small freebsd update (use strmode).
  * added -e switch.


Changes in 1.10
===============

  * fixed byte rage header parser to deal correctly with 64bit off_t.


Changes in 1.9
==============

  * added pidfile support.


Changes in 1.8
==============

  * added TCP_CORK support.


Changes in 1.7
==============

  * one more security fix (drop secondary groups).
  * catch malloc() failures in ls.c.


Changes in 1.6
==============

  * security fix (parsing option '-n' did unchecked strcpy).
  * documentation updates.


Changes in 1.5
==============

  * fixed the sloppy usage of addrlen for the ipv6 name lookup
    functions.  Linux worked fine, but the BSD folks have some
    more strict checks...
  * allow to write the access log to stdout (use "-" as filename)


Changes in 1.4
==============

  * fixed a bug in the base64 decoder (which broke basic auth for some
    user/passwd combinations)
  * added virtual host support.
  * webfsd can chroot to $DOCUMENT_ROOT now.


Changes in 1.3
==============

  * overwrite the -b user:pw command line option to hide the password
    (doesn't show up in ps anymore)


Changes in 1.2
==============

  * added ipv6 support.
  * bugfix in logfile timestamps.


Changes in 1.1
==============

  * added basic authentication (one username/password for all files)


Changes in 1.0
==============

  * added some casts to compile cleanly on Solaris.
  * new -F flag (don't run as daemon).


Changes in 0.9
==============

  * fixed a quoting bug.
  * documentation updates, minor tweaks.


Changes in 0.8
==============

  * fixed a bug in the directory cache.
  * fixed uncatched malloc()/realloc() failures.
  * added optional pthreads support.  Edit the Makefile to turn
    it on.


Changes in 0.7
==============

  * some portability problems fixed (0.6 didn't compile on FreeBSD).
  * added a sendfile() emulation based on read()/write() as fallback
    if there is no sendfile() available.
  * bugfix: '#' must be quoted too...


Changes in 0.6
==============

  * increased the listen backlog.
  * optionally flush every logfile line to disk.
  * new switch to specify the location of the mime.types file.
  * byte range bug fixes.
  * switch for the hostname has been changed ('-s' => '-n').
  * optional log errors to the syslog (switch '-s').
  * added sample start/stop script for RedHat.


Changes in 0.5
==============

  * FreeBSD port (Charles Randall <crandall@matchlogic.com>)
  * minor tweaks and spelling fixes.


Changes in 0.4
==============

  * last-modified headers (and 304 responses) for directory listings.
  * new switch: -f index.html (or whatever you want to use for
    directory indices)
  * killed the access() system calls in the ls() function.
  * added cache for user/group names.
  * wrote a manual page.


Changes in 0.3
==============

  * multipart/byteranges improved:  You'll get a correct Content-length:
    header for the whole thing, and we can handle keep-alive on these
    requests now.
  * bugfix: catch accept() failures.
  * bugfix: quote the path in 302 redirect responses.
  * accept absolute URLs ("GET http://host/path HTTP/1.1")
  * fixed handling of conditional GET requests (hope it is RFC-Compilant
    now...).
  * bugfix: '+' must be quoted using %xx.


Changes in 0.2
==============

  * added URL quoting.
  * root can set uid/gid now.
  * webfs ditches any setuid/setgid priviliges after binding to the
    TCP port by setting effective to real uid/gid.  It should be safe
    to install webfsd suid root to allow users to use ports below
    1024 (and _only_ this of course).  If anyone finds a flaw in this
    code drop me a note.
  * more verbose directory listing.
  * added logging. It does the usual logfile reopen on SIGHUP.


Changes in 0.1
==============

  * first public release.
