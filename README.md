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
in a new unified coherent virtual filesystem.

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
```
(base os)                              -> layer 1
   \_ installing patched nginx         -> layer 2
        \_ install users               -> layer 3
        |_ giving sudo for ops         -> layer 3 (snapshot as ops-nginx)
```

**step 2**
```
(ops-nginx)                 -> layer 1 (snapshot)
  \_ nginx website conf     -> layer 2 
  |_ ssl conf               -> layer 2
  |_ log conf               -> layer 2 (snapshot as website-nginx)
```




Few features that w.r.t to images docker supports
* download a docker image
* `exec` or `run` a command in that image
* save that image incase you have made some changes
* upload the image back

To make an OS start running "magically", you of course need an OS
image. Docker uses CoW (copy on write) filesystem or 
union filesystem [UnionFS](http://en.wikipedia.org/wiki/UnionFS). 

Lets take an example of how this works
 * download a base image
 * 
 *

# Kernel Namespaces

# cgroups

# Networking


