# fuse-file-system
File system implemented in Linux user mode

## Install
./configure
make
make install
modprobe fuse

You may also need to add '/usr/local/lib' to '/etc/ld.so.conf' and/or
run ldconfig.

Linux kernels 2.6.14 or later contain FUSE support out of the box.  If
FUSE support is detected, the kernel module in this package will not
be compiled.  It is possible to override this with the
'--enable-kernel-module' configure option.

If './configure' cannot find the kernel source or it says the kernel
source should be prepared, you may either try

  ./configure --disable-kernel-module

or if your kernel does not already contain FUSE support, do the
following:

  - Extract the kernel source to some directory

  - Copy the running kernel's config (usually found in
    /boot/config-X.Y.Z) to .config at the top of the source tree

  - Run 'make prepare'

Configuration
=============

Some options regarding mount policy can be set in the file
'/etc/fuse.conf'

Currently these options are:

mount_max = NNN

  Set the maximum number of FUSE mounts allowed to non-root users.
  The default is 1000.

user_allow_other

  Allow non-root users to specify the 'allow_other' or 'allow_root'
  mount options.


How To Use
==========

FUSE is made up of three main parts:

 - A kernel filesystem module

 - A userspace library

 - A mount/unmount program


Here's how to create your very own virtual filesystem in five easy
steps (after installing FUSE):

  1) Edit the file example/fusexmp.c to do whatever you want...

  2) Build the fusexmp program

  3) run 'example/fusexmp /mnt/fuse -d'

  4) ls -al /mnt/fuse

  5) Be glad

If it doesn't work out, please ask!  Also see the file 'include/fuse.h' for
detailed documentation of the library interface.
