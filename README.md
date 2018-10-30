## Patches for file-roller.

These patches apply to file-roller used as a workaround solution for gbk encoding detection.

### Apply patches

``` shell
$ git am *.patch
```

### Branches

 - **master**: patch files.
 - **upstream**: upstream state when generating these patches.
 - **patched**: code which applied the patches to *upstream* branch.

### Generating patches

``` shell
$ git format-patch <upstream_head_commit_hash>
```

### Appendix

The `0001-guess-zip-charset.patch` patch is generated from [snyh's codebase](https://github.com/snyh/fileroller/commits/master) with some modification. These patch are used for the file-roller package for [Deepin](https://distrowatch.com/deepin) (a Debian-based distribution).

Patches released as the [same license (GNU General Public License v2.0.)](https://gitlab.gnome.org/GNOME/file-roller/blob/master/COPYING) as the original file-roller.
