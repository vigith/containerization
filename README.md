# containerization

Understanding how containers work. I was quite baffled first time when
I saw docker running ubuntu on an amazon linux. I thought I will jot
down all I understand about `docker` (may be `rkt` in future) so someone
can save some time without needing to read a whole lot of documents.

**NOTE** : As of today this document references docker

# Basic Docker

I assume the reader knows more about Docker and its features. Knowing
the features of Docker will help the reader understand the internals
better (otherwise you might think why the heck am I saying this)

## Very Basic Docker

```
FROM ubuntu

CMD ["bash"]
```

If you save the above contents and do a `docker build -t vigith/ubuntu .` followed by a
`docker run -t -i ubuntu /bin/bash` you will get a `bash` promt in
`ubuntu` (you can confirm this by doing a `cat /etc/lsb-release` ).


# Images

Few features that docker supports
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


