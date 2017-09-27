# ReRtree - R-tree Module for Redis

This module provides a scalable range indexing for Redis. R-tree allows quickly search for intersected ranges.

## Building

In order to use this module, build it using `make` and load it into Redis. (you may need to build Redis first)

`rertree.so` file will be compiled.

## Building Redis

https://redis.io/download

Download, extract and compile Redis with:

$ wget http://download.redis.io/releases/redis-4.0.2.tar.gz
$ tar xzf redis-4.0.2.tar.gz
$ cd redis-4.0.2
$ make

### Using

**Invoking redis with the module loaded**

```
$ redis-server --loadmodule /path/to/rertree.so
```

**Adding items to filter**

```
127.0.0.1:6379> rtree.add {key} {range_start} {range_end} {val}
127.0.0.1:6379> rtree.add rtreekey 1 2 3
OK
```

**Searched for intersected ranges, returns keys of the ranges**
```
127.0.0.1:6379> rtree.find {key} {range_start} {range_end}
127.0.0.1:6379> rtree.find rtreekey 1 2
1) (integer) 3
```
