# Understanding Containerization

I was quite baffled first time when I saw docker running centos on an amazon linux. 
I thought I will jot down all I understand about `docker` (may be `rkt` in future) so 
someone  can save some time without needing to read a whole lot of documents.

**NOTE** : As of today this document references docker, but it should
  be true for other systems too.

# Basic Docker

I assume the reader knows more about Docker and its features. Knowing
the features of Docker will help the reader understand the internals
better (otherwise you might think why the heck am I saying this)

## Very Basic Docker

```
FROM centos
```

If you save the above contents and do a `docker build -t vigith/centos .` followed by a
`docker run -t -i centos /bin/bash` you will get a `bash` promt in
`centos` (you can confirm this by doing a `cat /etc/system-release` on
your new prompt).


# Images

## UnionFS
[UnionFS](http://en.wikipedia.org/wiki/UnionFS) lets you overlay files
and directories of different filesystem be overlaid forming a single
filesystem. The merged single filesystem will have no duplicates and
later layers take precedence over former layers such that we will end up
in a new unified coherent virtual filesystem. Couple of well know UnionFS
are _AUFS_, _btrfs_, _DeviceMapper_, _OverlayFS_, etc

### CopyOnWrite
It allows both read-only and read-write filesystems to be merged. Once 
a write is made, it can be persisted by making the copy go to a file
system. The writes can be either discarded or persisted, persisting
will enable users to create a snapshot of the changes and later build 
layers on top if it as if it were the base layer.

### An Usecase

You have to install nginx for you website. The end container on your
website server will be an nginx process tailor cut for website called
website-nginx. You would also like to reuse your nginx build because
it has lot of patches made specific for your env.

This can be done in 2 steps
* getting a specific version of nginx (patched with all crazy stuff)
  called `ops-nginx`
* use `ops-nginx` to build out `website-nginx` server, also this same
  `ops-nginx` can be used for other servers by just putting the right
  confs

**step 1**
Create the ops-nginx image from base os, then can be reused later for
many other apps
```
(base os)                              -> layer 1
   \_ installing patched nginx         -> layer 2
        \_ install users               -> layer 3
        |_ giving sudo for ops         -> layer 3 (snapshot as ops-nginx)
```

**step 2**
Create the website-nginx image from ops-nginx, a server with a specific set of
configs and other packages
```
(ops-nginx)                 -> layer 1 (snapshot)
  \_ nginx website conf     -> layer 2 
  |_ ssl conf               -> layer 2
  |_ log conf               -> layer 2 (snapshot as website-nginx)
```


### Docker Way

**step 1**
Create the ops-nginx image from base os
* docker pull centos
* docker run -t -i centos /bin/bash
  * yum install nginx foobar
  * ... other crazy command ...
* docker commit -m "ops nginx image" IMAGE_ID ops-nginx

**step 2**
Create the website-nginx image from ops-nginx
* docker pull ops_nginx
* docker run -t -i ops_nginx /bin/bash
  * .. change your config ..
  * ... other voodoo stuff ...
* docker commit -m "ops nginx image" IMAGE_ID website-nginx

## HOWTO

If there was no docker, how could have we done this!. Docker or other tools might be doing
real different, **but** lets see couple of ways how we can do it.

To understand how we can do it, we need to understand the following
* [loop device](http://en.wikipedia.org/wiki/Loop_device) - mount file as a block device
* [sparse filesystem](http://en.wikipedia.org/wiki/Sparse_file) - efficiently use FS when it is mostly empty
* [device mapper](http://en.wikipedia.org/wiki/Device_mapper) - [Kernel Device Mapper Doc](https://www.kernel.org/doc/Documentation/device-mapper/) mapping physical block devices onto higher-level virtual block devices 
  * [snapshotting](https://www.kernel.org/doc/Documentation/device-mapper/snapshot.txt) - snapshot the state of a file system at a give time
  * [thin provisioning](https://www.kernel.org/doc/Documentation/device-mapper/thin-provisioning.txt) - allows many virtual devices to be stored on the same data volume

I will try give some crude examples on device mappers. These examples try to do 2 step snapshots

```
empty filesystem
 |
 +- load filesystem (snapshot 1)
     |
     +- edit filesystem (snapshot 2)
```

If we can achieve this, then we can do this repeatitively, also can make and persist any kind of changes at any level of snapshot.

**NOTE** I am not expert in these domains, only my curiosity lead me to read and write this document. (Other words, don't try in production)

### Snapshot

This is a crude HOWTO on a working example of device mapper snapshots.

#### Loop Device

```shell
# create a sparse 100G file
> truncate -s100G test.block

# create /dev/loop0
# -f will find an unused device and use it
# --show will print the device name
> losetup -f --show test.block 
```

Now we have `/dev/loop0` (my example is based on `loop0`, if `loop0` is not free do a `losetup -d /dev/loop0`) attached to `test.block` (file mounted as block device).

#### Create Origin and Snapshot targets

```shell
# create base target (1953125 = 1000 * 1000 * 1000 / 512)
# where 512 byte = 1 sector, and GB = 1000 * 1000 * 1000  (it would have been 1024
# if GiB was the unit)
> dmsetup create test-snapshot-base-real --table '0 1953125 linear /dev/loop0 0'

# create the cow snapshot target
# 390625 + 1953125 = 2343750 (== 1.2GB)
> dmsetup create test-snapshot-snap-cow --table '0 390625 linear /dev/loop0 1953125'
```

#### Populate the Origin Device
I downloaded a centos rootfs (actually I took a docker centos image and converted it to tar via docker2aci). This
centos tar is named as `centos-latest.tar`.

```shell
# format the orgin as an ext4 device
> mkfs.ext4 /dev/mapper/test-snapshot-base-real
# create a dir to mount the new ext4 fs
> mkdir -p /mnt/loados
# mount it
> mount /dev/mapper/test-snapshot-base-real /mnt/loados
# load centos to new ext4
> tar -xf centos-latest.tar -C /mnt/loados/
# umount the dir
> umount /mnt/loados
```

#### Mark the device as Origin

We will make the newly created `ext4` filesystem containing `centos rootfs` as our origin.

```shell
# make /dev/mapper/test-snapshot-base-real as origin 
> dmsetup create test-snapshot-base --table '0 1953125 snapshot-origin /dev/mapper/test-snapshot-base-real'
```

#### Create CoW Snapshot

This will make a snapshot target, which can be mounted and edited. Snapshot target will be having the origin as its backend (ie,
if no write is made to snapshot `origin == snapshot`, else all new writes will go to snapshot)

```shell
# P (2nd last arg) means, make it persistent across reboot
# 8 (last arg) chunk-size, granularity of the of copying the snapshot
> dmsetup create test-snapshot-cow --table '0 1953125 snapshot /dev/mapper/test-snapshot-base-real /dev/mapper/test-snapshot-snap-cow P 8'
```

Note how the origin device is the not the same device as the one we just created (ie `test-snapshot-base`), but rather the origin's
underlying device `test-snapshot-base-real`

At this point if you do a `dmsetup status` you will see something as follows

```shell
## dmsetup status
> test-snapshot-snap-cow: 0 390625 linear
> test-snapshot-base: 0 1953125 snapshot-origin
> test-snapshot-base-real: 0 1953125 linear
> test-snapshot-cow: 0 1953125 snapshot 16/390625 16
```

#### Editing on CoW Snapshot

Lets add some data on the CoW Snapshot. The origin won't have these changes but only the CoW snapshot.

```shell
# mount the CoW device
> mount /dev/mapper/test-snapshot-cow /mnt/loados
# create a dir (one way to edit)
> mkdir /mnt/loados/rootfs/vigith_test
# add some data
> echo bar > /mnt/loados/rootfs/vigith_test/foo
# umount the device
> umount /mnt/loados
```

#### Merging the Snapshot

Take the changes we have made and merge these changes to the origin, so the origin will have all these changes.
This is good to do because, next time we create a snapshot we will already have the changes.

To merge a snapshot,
* origin must be suspended
* the snapshot device unmapped
* merge the snapshot via `snapshot-merge`
* resume
* once merge is complete (check it via `dmsetup status`)
* suspend; replace the snapshot-origin with snapshot-merge; reload

```shell
## replace the snapshot-origin target replaced with the snapshot-merge target, and the origin resumed
> dmsetup suspend test-snapshot-base
> dmsetup remove test-snapshot-cow
> dmsetup reload test-snapshot-base --table '0 1953125 snapshot-merge /dev/mapper/test-snapshot-base-real /dev/mapper/test-snapshot-snap-cow P 8'
```

if you do a `dmsetup status` you will see that `test-snapshot-cow` is missing now.

```shell
## dmsetup status
> test-snapshot-snap-cow: 0 390625 linear
> test-snapshot-base: 0 1953125 snapshot-origin  <--- it is snapshot-origin
> test-snapshot-base-real: 0 1953125 linear
```

do a resume
```shell
> dmsetup resume test-snapshot-base
```

If you do `dmsetup status`, you will see that `snapshot-origin` became `snapshot-merge`

```shell
> dmsetup status
test-snapshot-snap-cow: 0 390625 linear
test-snapshot-base: 0 1953125 snapshot-merge 16/390625 16  <--- snapshot-merge
test-snapshot-base-real: 0 1953125 linear 
```

suspend; replace the snapshot-origin with snapshot-merge; reload

```shell
## dmsetup status output will need be polled to find out then the merge is complete.
## Once the merge is complete, the snapshot-merge target should be replaced with the snapshot-origin target
> dmsetup suspend test-snapshot-base
> dmsetup reload test-snapshot-base --table '0 1953125 snapshot-origin /dev/mapper/test-snapshot-base-real'
> dmsetup resume test-snapshot-base
```

Now `dmsetup status` will confirm that `snapshot-merge` has become `snapshot-origin`

```shell
> dmsetup status
test-snapshot-snap-cow: 0 390625 linear
test-snapshot-base: 0 1953125 snapshot-origin    <--- snapshot-origin
test-snapshot-base-real: 0 1953125 linear
```

#### Load snapshot-origin to check for merge

We should be seeing the new directory we created in here `/mnt/loados/rootfs/vigith_test` and also the file inside
that dir `/mnt/loados/rootfs/vigith_test/foo`

```shell
# mount
> mount /dev/mapper/test-snapshot-base /mnt/loados
# you should be seeing 'bar' as output
> cat /mnt/loados/rootfs/vigith_test/foo
bar
# unmount it
> umount /mnt/loados
```

#### file based FileSystem

If you remember, we start with a file called `test.block`. If you run `file test.block` or `tune2fs -l test.block` you will see it is
an `ext4` file. Also, you can mount that file to any dir and you will see that it is the merged origin you just created

```shell
# run file
> file test.block
# tune2fs
> tune2fs -l test.block
# create a mount dir
> mkdir /tmp/testmnt
# lets mount this test.block
> mount -o loop test.block /tmp/testmnt
# look for the dir and file we created  
> cat /tmp/testmnt/rootfs/vigith_test/foo
bar
# umount it
> umount /tmp/testmnt
```

Now you have a file that can be mounted.

An astute reader might say, ofcourse you can add files and manipulate the FS but what about installing packages,
compiling source code pointing to new libraries in the new FS. Answer to that is, _keep reading_, skip to next
section if you are really curious.

### Thin Provisioning

Compared to the previous implementation of snapshots, is that it allows many virtual devices to
be stored on the same data volume. Please read the [doc](https://www.kernel.org/doc/Documentation/device-mapper/thin-provisioning.txt) more to understand about it.

An example of how thin provisioning works.

#### Loop Device

**Thin Provisioning** requires a metadata and data store.

```shell
# create a sparse 100G data file
> truncate -s100G testthin.block
# create a sparse 1G metadata file
> truncate -s1G testmetadata.block

# create /dev/loop0
# -f will find an unused device and use it
# --show will print the device name
> losetup -f --show testthin.block
# create /dev/loop1 for metadata
> losetup -f --show testmetadata.block
# clean it with zeros
> dd if=/dev/zero of=/dev/loop1 bs=4096 count=1
```

#### Create a Thin Pool

```shell
# test-thin-pool => poolname
# /dev/loop1 /dev/loop0  => metadata and data devices
# 20971520 => 10GiB (20971520 = 10 * 1024 * 1024 * 1024 / 512)
# 128 => data blocksize 
> dmsetup create test-thin-pool --table '0 20971520 thin-pool /dev/loop1 /dev/loop0 128 0'
```

#### Create a Thin Volume

* send message to active pool device
* activate the new volume (allocate storage)

```shell
# create a new thin volume
# 0 (last arg) => 24 bit identifier
# 0 (other 0) => sector (512 bytes) in the logical device
> dmsetup message /dev/mapper/test-thin-pool 0 "create_thin 0"

# allocate storage/activate
# 0  (last arg) => thinp device identifier
# 2097152 => 1GiB (2097152 sectors = 1024 * 1024 * 1024 / 512)
> dmsetup create test-thin --table '0 2097152 thin /dev/mapper/test-thin-pool 0'
```

#### Load Data

Load the data to the new thin device. We will use this loaded thin device to
create snapshots.

```shell
# create an ext4 partition
> mkfs.ext4 /dev/mapper/test-thin
# mount the dir
> mount /dev/mapper/test-thin /mnt/loados
# load the partition with centos
> tar -xf centos-smaller.tar -C /mnt/loados/
# unmount it
> umount /mnt/loados/
```

#### Internal Snapshot

* suspend the origin device whose snapshot is being taken
* send message "create_snap"
* resume the origin device

```shell
# suspend origin
> dmsetup suspend /dev/mapper/test-thin

# create snapshot
# 1 => identifier for snapshot
# 0 => identifier for origin device (last arg 0)
> dmsetup message /dev/mapper/test-thin-pool 0 "create_snap 1 0"

#resume the origin
> dmsetup resume /dev/mapper/test-thin
```

If you do an `ls -l /dev/mapper` you won't be seeing any snapshot yet.

```shell
> ls /dev/mapper/
control  test-thin  test-thin-pool
```

#### Activating Internal Snapshot

Once created, the user doesn't have to worry about any connection between the origin and the snapshot.
It can be worked on like yet another thinly-provisioned device (ie, you can do snapshots on this)

```shell
# active the snapshot (note there that we gave 1)
# 1 => snapshot identifier (same value we gave when we called "create_snap")
# 2097152 => 1GiB (2097152 sectors = 1024 * 1024 * 1024 / 512)
> dmsetup create snap --table "0 2097152 thin /dev/mapper/pool 1"
```

If you do a `ls -l /dev/mapper` you should be seeing `test-thin-snap` in the listing

```shell
> ls /dev/mapper/
control  test-thin  test-thin-pool  test-thin-snap
```

#### Editing Snapshot

Lets mount this thin snapshot and put some data

```shell
# mount
> mount /dev/mapper/test-thin-snap /mnt/loados
# create some new dir
> mkdir /mnt/loados/rootfs/vigith
# write some data
> echo bar > /mnt/loados/rootfs/vigith/foo
# umount
> umount /mnt/loados/
```

#### Internal Snapshot (Again)

This snapshot is exactly same as the earlier discussed _Internal Snapshot_. 

```shell
# suspend the origin (origin for this snap, but it is a snapshot of 1st origin)
> dmsetup suspend /dev/mapper/test-thin-snap
# please note we have incremented identifier to 2 and origin is 1
# (for the earlier run it was 1 and 0)
> dmsetup message /dev/mapper/test-thin-pool 0 "create_snap 2 1"
# resume the origin
> dmsetup resume /dev/mapper/test-thin-snap
```

#### Activating Internal Snapshot (Again)

Same as activating the earlier _Activating Internal Snapshot_, except that the identifier is 2 now (it was 1 before)

```shell
# earlier the identifier was 1
# lets call it test-thin-snap-2
> dmsetup create test-thin-snap-2 --table '0 2097152 thin /dev/mapper/test-thin-pool 2'
```

#### Load Snapshot

Load the latest snapshot to see the new dir created

```shell
> mount /dev/mapper/test-thin-snap-2 /mnt/loados
> ls -l /mnt/loados/rootfs/vigith/foo
# you should be seeing 'bar' as output
> cat /mnt/loados/rootfs/vigith/foo
bar
> umount /mnt/loados
```

#### file based FileSystem

If you remember, we start with a file called `testthin.block`. If you run `file testthin.block` or `tune2fs -l testthin.block`
you will see it is an `ext4` file. Also, you can mount that file to any dir and you will see that it is the merged origin
you just created.

```shell
# run file
> file test.block
# tune2fs
> tune2fs -l test.block
# create a mount dir
> mkdir /tmp/testmnt
# lets mount this test.block
> mount -o loop test.block /tmp/testmnt
# look for the dir and file we created
> cat /tmp/testmnt/rootfs/vigith_test/foo
bar
# umount it
> umount /tmp/testmnt
```

The metadata stored in `testmetadata.block` is of on much use to us (or maybe, i am just unaware)

# Kernel Namespaces

`clone` syscall, really! that is all about kernel namespaces.

Using Kernel Namespaces, we achieve process isolation 

* ipc - InterProcess Communication (flag: `CLONE_NEWIPC`)
* mnt - Mount points (flag: `CLONE_NEWNS`)
* pid - Process ID  (flag: `CLONE_NEWPID`)
* net - Networking (flag: `CLONE_NEWNET`)
* uts - set of identifiers returned by `uname(2)` (flag: `CLONE_NEWUTS`)

## clone syscall

When a process is created, the new process inherits most of the parent process flags. To use
namespaces we just need to pass the right flags.

```c
int clone(int (*fn)(void *), void *child_stack,
   int flags, void *arg, ...
   /* pid_t *ptid, struct user_desc *tls, pid_t *ctid */ );
```

The 3rd argument is the flags.

For eg, `clone(fn_child, child_stack, CLONE_NEWPID|CLONE_NEWNET, &fn_child_args);` can be called to create
a child process with a new `net` and `pid` namespace.

### Clone /bin/bash

To understand Kernel Namespaces, lets write a sample clone code and start building on it. The major change
will be in `static int clone_flags = SIGCHLD;` where we will add more flags. This code when executed will
run a new `bash` process in the child context.

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <string.h>

#define STACKSIZE (1024*1024)

/* the flags */
static int clone_flags = SIGCHLD;

/* fn_child_exec is the func that will be executed by clone
   and when this function returns, the child process will be
   terminated */
static int fn_child_exec(void *arg) {
  char * const cmd[] = { "/bin/bash", NULL};
  fprintf(stderr, "Child Pid: [%d] Invoking Command [%s] \n", getpid(), cmd[0]);
  if (execv(cmd[0], cmd) != 0) {
    fprintf(stderr, "Failed to Run [%s] (Error: %s)\n", cmd[0], strerror(errno));
    exit(-1);
  }
  /* exec never returns */
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv) {
  char *child_stack = (char *)malloc(STACKSIZE*sizeof(char));

  /* create a new process, the function fn_child_exec will be called */
  pid_t pid = clone(fn_child_exec, child_stack + STACKSIZE, clone_flags, NULL);
    
  if (pid < 0) {
    fprintf(stderr, "clone failed (Reason: %s)\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  /* lets wait on our child process here before we, the parent, exits */
  if (waitpid(pid, NULL, 0) == -1) {
    fprintf(stderr, "'waitpid' for pid [%d] failed (Reason: %s)\n", pid, strerror(errno));
    exit(EXIT_FAILURE);
  }

  return 0;
}
```

To compile, save this code (TODO: save this as a git code) as `clone_example.c`

```shell
> gcc clone_example.c -o bash_ex
```

Now when you run `./bash_ex`, you will get a new bash child process.

```shell
> ./bash_ex 
Child Pid: [13225] Invoking Command [/bin/bash]
```

### ipc

`man` page of clone describes the `CLONE_NEWIPC` flag as below

```
If  CLONE_NEWIPC  is set, then create the process in a new IPC namespace.  If this flag is not set,
then (as with fork(2)), the process is created in the same IPC namesace as the calling process.
This flag is intended for the implementation of containers.
```

Recompile the code after changing `static int clone_flags = SIGCHLD;` **to** `static int clone_flags = SIGCHLD|CLONE_NEWIPC;`

We will create a Shared Memory Segment in the parent shell and we will confirm that we can see the segment
we created

```shell
# create a segment
> ipcmk -M 4096

# list the shared memory segments
> ipcs -m
------ Shared Memory Segments --------
key        shmid      owner      perms      bytes      nattch     status
0x6b68f06d 0          ec2-user   644        4096       0              
```

Now run the `./bash_ex` you just created (with the new flag), now when you do `ipcs -m`, you won't be seeing the
segments created earlier, because you are in a new IPC Namesace.

```shell
# cloned process with CLONE_NEWIPC set
> ./bash_ex
Child Pid: [12624] Invoking Command [/bin/bash]

## no shared memory listed
> ipcs -m
------ Shared Memory Segments --------
key        shmid      owner      perms      bytes      nattch     status      
```

### mnt

`CLONE_NEWNS` creates a new mount namespace. If the child process you created has set `CLONE_NEWNS`, then the `mount(2)` and
`umount(2)` system calls will only affect the child process (or processes that live in the same namespace). You can have
multiple process in same mount namespace if you create new child processes without setting `CLONE_NEWNS`.

Recompile the code after changing `static int clone_flags = SIGCHLD;` **to** `static int clone_flags = SIGCHLD|CLONE_NEWNS;`

Run the `./bash_ex`, we will `umount` `tmpfs` and prove that it only got removed in child process, not in the parent.

```shell
> ./bash_ex
Child Pid: [12785] Invoking Command [/bin/bash]
## list the mount type tmpfs
> mount -l -t tmpfs
tmpfs on /dev/shm type tmpfs (rw,relatime)
## unmount tmpfs
> umount tmpfs
## show that the tmpfs got unmounted
> mount -l -t tmpfs
```

While in the shell (or anything process) they will still see `tmpfs` mounted.

```shell
## tmpfs is still mounted
> mount -l -t tmpfs
tmpfs on /dev/shm type tmpfs (rw,relatime)
```

**NOTE**: Please don't mistake mount namespace with process jailing, this has nothing to do with jailing.

### pid

```
A PID namespace provides an isolated environment for PIDs: PIDs in a new namespace start at 1, somewhat like a standalone system, and calls to
fork(2), vfork(2), or clone()  will  produce processes with PIDs that are unique within the namespace. The  first  process  created  in  a new
namespace (i.e., the process created using the CLONE_NEWPID flag) has the PID 1, and is the "init" process for the namespace. Children that are
orphaned within the namespace will be reparented to this process rather than init(8).
```

Recompile the code after changing `static int clone_flags = SIGCHLD;` **to** `static int clone_flags = SIGCHLD|CLONE_NEWPID;`

Execute the code and check the pid of the process, it should be `1`

```shell
> ./bash_ex
Child Pid: [1] Invoking Command [/bin/bash]
> echo $$
1
```

If you do a `pstree` or `ps auxwww`, you will see lot of other processes too. This is because those tools work by reading
`/proc` dir and our `/proc` is still pointing to the parent process's namespace.

### net

Recompile the code after changing `static int clone_flags = SIGCHLD;` **to** `static int clone_flags = SIGCHLD|CLONE_NEWNET;`

If you do `ip addr` on the terminal you will be seeing multiple interfaces, like `lo`, `eth0` etc. Now execute the newly compiled
code and do an `ip addr` on the child bash promt, you will be seeing only `lo` interface.

`ip addr` on normal bash prompt

```shell
> ip addr
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
..snip..
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 9001 qdisc pfifo_fast state UP qlen 1000
    link/ether 0a:a5:84:25:0a:db brd ff:ff:ff:ff:ff:ff
..snip..
```

`ip addr` on bash process created with `CLONE_NEWNET`

```shell
> ./bash_ex
Child Pid: [13182] Invoking Command [/bin/bash]
## only lo is shown
> ip addr
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
..snip..
```

### uts

```
A UTS namespace is the set of identifiers returned by uname(2); among these, the domain name and the host name can be modified
by setdomainname(2) and sethostname(2),  respectively Changes made to the identifiers in a UTS namespace are visible to all
other processes in the same namespace, but are not visible to processes in other UTS namespaces.
```

Recompile the code after changing `static int clone_flags = SIGCHLD;` **to** `static int clone_flags = SIGCHLD|CLONE_NEWUTS;`

We should be able to change the hostname in the new process and still not affect the hostname of the global namespace. 

```shell
> ./bash_ex
Child Pid: [13225] Invoking Command [/bin/bash]
# change the hostname
> hostname foo.bar
> hostname
foo.bar
```

While the hostname as per the global namespace is still unaltered. 
```shell
## hostname of the system
> hostname
test.qa
```

# Process Containers (cgroups)

# Networking

* veth - Virtual Ethernet device that comes in as a pair device, anything that is send to one device will come out from the other
* bridge - Virtual Ethernet Bridge Device
* netns - Network Namespace

## Inter Container Communication

```
+--------------+                                     +--------------+
|              |                                     |              |
| Container 1 (iface)  <====== (bridge) ======> (iface) Container 2 |
|              |                                     |              |
+--------------+                                     +--------------+
```

* create a bridge
* activate the bridge
* create a vethA pair (vethA1/vethA2)
* set bridge as the master to one end of the veth pair (vethA1)
* bring the vethA1 up
* attach the vethA2 to your container namespace (container1)
  * name the vethA2 interface as eth1 (optional)
  * give ip addr to interface (use the new name or use vethA2)
  * bring up the interface
  * do an `arping` to make sure interface is good
* create another veth pair (vethB1/vethB2)
* set bridge as the master to one end of the veth pair (vethB1)
* bring the vethB1 up
* attach the vethB2 to your other container namespace  (container2)
  * name the vethB2 interface as eth1 (optional)
  * give ip addr to interface (use the new name or use vethB2)
  * bring up the interface
  * do an `arping` to make sure interface is good


### Example

Start two process with new network namespace. In case you forgot HOWTO, 
recompile the template code we wrote for Kernel Namespaces after changing
`static int clone_flags = SIGCHLD;` **to** `static int clone_flags = SIGCHLD|CLONE_NEWNET;`
When you execute the compiled binary, you will get two process in two differen network namespaces.

By default there two comtainers won't have any interface attached other than `lo`, so they won't be
able to talk to each other. Lets make them talk!

**Start Process 1** (prompt 1)
```shell
# process 1
> ./bash_ex
Child Pid: [2264] Invoking Command [/bin/bash]
```

**Start Process 2** (prompt 2)
```shell
# process 2
> ./bash_ex
Child Pid: [2307] Invoking Command [/bin/bash]
```

**On the Global Namespace**
```shell
# to make life easier, lets set the 2 pids as our
# namespaces
# (pid from prompt1)
> pidA=2264
# (pid from prompt2)
> pidB=2307

# make it ready for `ip netns` to read
# (strace told me so)
> mkdir -p /var/run/netns
> ln -s /proc/$pidA/ns/net /var/run/netns/$pidA
> ln -s /proc/$pidB/ns/net /var/run/netns/$pidB

# create the bridge
> ip link add dev br1 type bridge

# bring up the bridge
> ip link set br1 up

# veth pair I
# mtu can be fetched by calling (ip link show br1)
> ip link add name vethA1 mtu 1500 type veth peer name vethA2 mtu 1500

# enslave vethA1 to br1
> ip link set vethA1 master br1
# bring vethA1 up
> ip link set vethA1 up

# attach other end of veth to a namespace
> ip link set vethA2 netns $pidA
# rename vethA2 to eth1 (optional)
> ip netns exec $pidA ip link set vethA2 name eth1
# attach an ipaddr to to the interface
> ip netns exec $pidA ip addr add 192.168.1.1/24 dev eth1
# bring the interface ip
> ip netns exec $pidA ip link set eth1 up
# test by an arping
> ip netns exec $pidA arping -c 1 -A -I eth1 192.168.1.1
ARPING 192.168.1.1 from 192.168.1.1 eth1
Sent 1 probes (1 broadcast(s))
Received 0 response(s)

# veth pair II
> ip link add name vethB1 mtu 1500 type veth peer name vethB2 mtu 1500

# enslave vethB1 to br1
> ip link set vethB1 master br1
# bring vethB1 up
> ip link set vethB1 up

# attach vethB to a namespace
> ip link set vethB2 netns $pidB
# rename to eth1 (optional)
> ip netns exec $pidB ip link set vethB2 name eth1
# attach an ipaddr to interface
> ip netns exec $pidB ip addr add 192.168.1.2/24 dev eth1
# bring the interface up
> ip netns exec $pidB ip link set eth1 up
# arping test
> ip netns exec $pidB arping -c 1 -A -I eth1 192.168.1.2
ARPING 192.168.1.2 from 192.168.1.2 eth1
Sent 1 probes (1 broadcast(s))
Received 0 response(s)

# remove the stuffs we brought in
> unlink /var/run/netns/$pidA
> unlink /var/run/netns/$pidB
> rmdir /var/run/netns
```

#### Testing the Setup

In the prompt you created (that is how you got the PID). Try to do a connect.

**Prompt 1**
```shell
> ./bash_ex
Child Pid: [2264] Invoking Command [/bin/bash]
> ip addr
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
4: eth1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP qlen 1000
    link/ether 6a:8f:4a:61:97:90 brd ff:ff:ff:ff:ff:ff
    inet 192.168.1.1/24 scope global eth1
..snip..
# list on one container
> nc -l 1234
hi
```

**Prompt 2**
```shell
> ./bash_ex
Child Pid: [2264] Invoking Command [/bin/bash]
> ip addr
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
6: eth1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP qlen 1000
    link/ether 1e:44:08:bf:a5:ad brd ff:ff:ff:ff:ff:ff
    inet 192.168.1.2/24 scope global eth1
..snip..
# talk to the other container 
> echo "hi" | nc 192.168.1.1 1234
> 
```


To poke into your docker network namespace `netns`, you need to link to `/var/run/netns` and use `ip netns exec`
(ip command looks into the var dir to get info). [man(7) namespaces](http://man7.org/linux/man-pages/man7/namespaces.7.html) has more
info about the `/proc/$pid/ns` namespaces.

eg,
```shell
# it might be missing
> mkdir -p /var/run/netns

# link the process in the container
# you should find $pid
> ln -s /proc/$pid/ns/net /var/run/netns/$pid

# list your namespaces (it should return the pid)
> ip netns ls
<pid>

# list the interfaces in 
> ip netns exec <pid> ip addr
....info about interface in the namespace <pid> ...

# please remove what you did after you are done with the experiments
> unlink /var/run/netns/$pid
# don't force delete, someone else too might be mucking around :-)
> rmdir /var/run/netns
```

Curious mind can do `tcpdump -i br0 -n ` (provided br0 is your bridge name) to see packets going back and
forth.

## Host to Container Communication

```
+---------------------------------------+
|                                       |
|   +-----------+                       |
|   |           |                       |
|   | Container |                       |
|   |          (iface) <===== bridge    |
|   +-----------+               ^       |
|                               |       |
|  Host                        (iface)  |
+---------------------------------------+
```

* create a bridge
* activate the bridge
* create a vethA pair (vethA1/vethA2)
* set bridge as the master to one end of the veth pair (vethA1)
* bring the vethA1 up
* attach the vethA2 to your container namespace (container1)
  * name the vethA2 interface as eth1 (optional)
  * give ip addr to interface (use the new name or use vethA2)
  * bring up the interface
  * do an `arping` to make sure interface is good
  * add routing entry from container to bridge
* assign ip addr to bridge
* add routing entry from host to container (via bridge)

We need to attach one end of the veth pair to the container while the other end to the global namespace. Here we
need to manually assign ip to the bridge `br1` and also add the routing table entry from host to the container
and also add a routing entry back from container to host (via veth endpoint)

**Start Process 1** (promt 1)
```shell
> ./bash_ex
Child Pid: [2264] Invoking Command [/bin/bash]
```

**On Global Namespace**
```shell
# pid from the prompt1
> pidA=2264

# make network namespace visible to `ip netns`
> mkdir -p /var/run/netns
> ln -s /proc/$pidA/ns/net /var/run/netns/$pidA

# setup the veth
> ip link add dev br1 type bridge
> ip link set br1 up

> ip link add name vethA1 mtu 1500 type veth peer name vethA2 mtu 1500
> ip link set vethA1 master br1
> ip link set vethA1 up
> ip link set vethA2 netns $pidA

> ip netns exec $pidA ip link set vethA2 name eth1
> ip netns exec $pidA ip addr add 192.168.1.1/24 dev eth1
> ip netns exec $pidA ip link set eth1 up
> ip netns exec $pidA arping -c 1 -A -I eth1 192.168.1.1
ARPING 192.168.1.1 from 192.168.1.1 eth1
Sent 1 probes (1 broadcast(s))
Received 0 response(s)
# route entry back to the host
> ip netns exec $pidA ip route add 192.168.2.0/24 dev eth1 proto kernel  scope link src 192.168.1.1

# add ip addr to bridge
> ip addr add 192.168.2.1/24 dev br1
# add a route entry
> ip route add 192.168.1.0/24 dev br1 proto kernel  scope link src 192.168.2.1 
```

