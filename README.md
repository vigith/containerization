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

### An Example usecase

You have to install nginx for you website. The end container on your
website server will be an nginx process tailor cut for website called
website-nginx.

This can be done in 2 steps
* getting a specific version of nginx (patched with all crazy stuff)
  called `ops-nginx`
* use `ops-nginx` to build out rest of the nginx based servers with
  just changing the conf

**step 1**
Create the ops-nginx image from base os
```
(base os)                              -> layer 1
   \_ installing patched nginx         -> layer 2
        \_ install users               -> layer 3
        |_ giving sudo for ops         -> layer 3 (snapshot as ops-nginx)
```

**step 2**
Create the website-nginx image from ops-nginx
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
* [device mapper](http://en.wikipedia.org/wiki/Device_mapper) - mapping physical block devices onto higher-level virtual block devices [Kernel Device Mapper Doc](https://www.kernel.org/doc/Documentation/device-mapper/)
  * [snapshotting](https://www.kernel.org/doc/Documentation/device-mapper/snapshot.txt) - snapshot the state of a file system at a give time
  * [thin provisioning](https://www.kernel.org/doc/Documentation/device-mapper/thin-provisioning.txt) - allows many virtual devices to be stored on the same data volume

**NOTE** I am not expert in these domains, only my curiosity lead me to read and write this document. (Other words, don't try in production)

### Snapshot

An example of how snapshot works. 

#### Loop Device

```
# create a sparse 100G file
truncate -s100G test.block

# create /dev/loop0
# -f will find an unused device and use it
# --show will print the device name
losetup -f --show test.block 
```

Now we have `/dev/loop0` (my example is based on `loop0`, if `loop0` is not free do a `losetup -d /dev/loop0`) attached to `test.block` (file mounted as block device).

#### Create Origin and Snapshot targets

```
# create base target (1953125 = 1000 * 1000 * 1000 / 512)
# where 512 byte = 1 sector, and GB = 1000 * 1000 * 1000  (it would have been 1024
# if GiB was the unit)
dmsetup create test-snapshot-base-real --table '0 1953125 linear /dev/loop0 0'

# create the cow snapshot target
# 390625 + 1953125 = 2343750 (== 1.2GB)
dmsetup create test-snapshot-snap-cow --table '0 390625 linear /dev/loop0 1953125'
```

#### Populate the Origin Device
I downloaded a centos rootfs (actually I took a docker centos image and converted it to tar via docker2aci). This
centos tar is named as `centos-latest.tar`.

```
# format the orgin as an ext4 device
mkfs.ext4 /dev/mapper/test-snapshot-base-real
# create a dir to mount the new ext4 fs
mkdir -p /mnt/loados
# mount it
mount /dev/mapper/test-snapshot-base-real /mnt/loados
# load centos to new ext4
tar -xf centos-latest.tar -C /mnt/loados/
# umount the dir
umount /mnt/loados
```

#### Mark the device as Origin

We will make the newly created `ext4` filesystem as our origin.

```
# make /dev/mapper/test-snapshot-base-real as origin 
dmsetup create test-snapshot-base --table '0 1953125 snapshot-origin /dev/mapper/test-snapshot-base-real'
```

#### Create CoW Snapshot

This will make a snapshot target, which can be mounted and edited. Snapshot target will be having the origin as its backend (ie,
if no write is made to snapshot `origin == snapshot`, else all new writes will go to snapshot)

```
# P (2nd last arg) means, make it persistent across reboot
# 8 (last arg) chunk-size, granularity of the of copying the snapshot
dmsetup create test-snapshot-cow --table '0 1953125 snapshot /dev/mapper/test-snapshot-base-real /dev/mapper/test-snapshot-snap-cow P 8'
```

Note how the origin device is the not the same device as the one we just created (ie `test-snapshot-base`), but rather the origin's
underlying device `test-snapshot-base-real`

At this point if you do a `dmsetup status` you will see something as follows

```
## dmsetup status
test-snapshot-snap-cow: 0 390625 linear
test-snapshot-base: 0 1953125 snapshot-origin
test-snapshot-base-real: 0 1953125 linear
test-snapshot-cow: 0 1953125 snapshot 16/390625 16
```

#### Editing on CoW Snapshot

Lets add some data on the CoW Snapshot. The origin won't have these changes but only the CoW snapshot.

```
# mount the CoW device
mount /dev/mapper/test-snapshot-cow /mnt/loados
# create a dir (one way to edit)
mkdir /mnt/loados/rootfs/vigith_test
# add some data
echo bar > /mnt/loados/rootfs/vigith_test/foo
# umount the device
umount /mnt/loados
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

```
## replace the snapshot-origin target replaced with the snapshot-merge target, and the origin resumed
dmsetup suspend test-snapshot-base
dmsetup remove test-snapshot-cow
dmsetup reload test-snapshot-base --table '0 1953125 snapshot-merge /dev/mapper/test-snapshot-base-real /dev/mapper/test-snapshot-snap-cow P 8'
```

if you do a `dmsetup status` you will see that `test-snapshot-cow` is missing now.

```
## dmsetup status
test-snapshot-snap-cow: 0 390625 linear
test-snapshot-base: 0 1953125 snapshot-origin  <--- it is snapshot-origin
test-snapshot-base-real: 0 1953125 linear
```

do a resume
```
dmsetup resume test-snapshot-base
```

If you do `dmsetup status`, you will see that `snapshot-origin` became `snapshot-merge`

```
### dmsetup status
test-snapshot-snap-cow: 0 390625 linear
test-snapshot-base: 0 1953125 snapshot-merge 16/390625 16  <--- snapshot-merge
test-snapshot-base-real: 0 1953125 linear 
```

suspend; replace the snapshot-origin with snapshot-merge; reload

```
## dmsetup status output will need be polled to find out then the merge is complete.
## Once the merge is complete, the snapshot-merge target should be replaced with the snapshot-origin target
dmsetup suspend test-snapshot-base
dmsetup reload test-snapshot-base --table '0 1953125 snapshot-origin /dev/mapper/test-snapshot-base-real'
dmsetup resume test-snapshot-base
```

Now `dmsetup status` will confirm that `snapshot-merge` has become `snapshot-origin`

```
## dmsetup status
test-snapshot-snap-cow: 0 390625 linear
test-snapshot-base: 0 1953125 snapshot-origin    <--- snapshot-origin
test-snapshot-base-real: 0 1953125 linear
```

#### Load snapshot-origin to check for merge

We should be seeing the new directory we created in here `/mnt/loados/rootfs/vigith_test` and also the file inside
that dir `/mnt/loados/rootfs/vigith_test/foo`

```
# mount
mount /dev/mapper/test-snapshot-base /mnt/loados
# do a cat (you should see 'bar' as output)
cat /mnt/loados/rootfs/vigith_test/foo
# unmount it
umount /mnt/loados
```

#### file based FileSystem

If you remember, we start with a file called `test.block`. If you run `file test.block` or `tune2fs -l test.block` you will see it is
an `ext4` file. Also, you can mount that file to any dir and you will see that it is the merged origin you just created

```
# run file
file test.block
# tune2fs
tune2fs -l test.block
# create a mount dir
mkdir /tmp/testmnt
# lets mount this test.block
mount -o loop test.block /tmp/testmnt
# look for the dir and file we created  
cat /tmp/testmnt/rootfs/vigith_test/foo
# umount it
umount /tmp/testmnt
```

Now you have a file that can be mounted.


# Kernel Namespaces

# cgroups

# Networking


